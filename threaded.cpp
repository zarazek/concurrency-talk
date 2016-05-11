#include <boost/asio.hpp>
#include <boost/date_time.hpp>
#include <boost/lexical_cast.hpp>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <deque>
#include <map>
#include <set>

#include <functional>

class ChatServer;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  ClientSession(ChatServer& server, boost::asio::io_service &ioService);

  boost::asio::ip::tcp::socket& socket() {
    return _socket;
  }

  const std::string* getName() const {
    return _nameValid ? &_name : nullptr;
  }

  void setName(const std::string &name) {
    assert(! _nameValid);
    _name = name;
    _nameValid = true;
  }

  void start();
  void sendMessage(const std::shared_ptr<std::string>& msg);
  void terminate();
  void waitToFinish();

private:
  void readerThread();
  std::string readLineFromClient();
  bool parseLine(const std::string& line);
  void writerThread();
  std::shared_ptr<std::string> getMessage();
  void interruptReader();

  void onReaderShutdown();
  void onWriterShutdown();

  enum {
    ALL_RUNNING = 0,
    READER_TERMINATED = 1,
    WRITER_TERMINATED = 2,
    READER_TERMINATION_REQUESTED = 4
  };

  ChatServer& _server;
  boost::asio::ip::tcp::socket _socket;
  std::string _name;
  bool _nameValid;
  boost::asio::streambuf _inputBuffer;
  std::mutex _messagesMutex;
  std::deque<std::shared_ptr<std::string> > _messages;
  std::condition_variable _writerCondition;
  std::thread _readerThread;
  std::thread _writerThread;
  std::atomic<int> _state;
};

template <class T>
struct PtrLess {
  bool operator()(const T *lhs, const T *rhs) const {
    if (lhs == nullptr) {
      return rhs != nullptr;
    }
    else if (rhs == nullptr) {
      return false;
    }
    else {
      return *lhs < *rhs;
    }
  }
};

class ChatServer {
public:
  ChatServer(int port);
  ~ChatServer();

  void run();

  bool setClientName(const std::shared_ptr<ClientSession>& client, const std::string &name);
  void broadcast(ClientSession& sender, const std::shared_ptr<std::string>& msg);
  void removeClient(std::shared_ptr<ClientSession>&& client);
  void shutdown();
private:
  void reaperThread();
  std::shared_ptr<ClientSession> getClientToRemove();

  typedef std::map<const std::string*,
		   std::shared_ptr<ClientSession>,
		   PtrLess<std::string> >  NamesToClientsMap;

  boost::asio::io_service _ioService;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::mutex _clientsMutex;
  std::set<std::shared_ptr<ClientSession> > _clients;
  std::mutex _namesToClientsMutex;
  NamesToClientsMap  _namesToClients;
  std::mutex _clientsToRemoveMutex;
  std::deque<std::shared_ptr<ClientSession> > _clientsToRemove;
  std::condition_variable _reaperCondition;
  std::thread _reaperThread;
  pthread_t _acceptingThreadId;
  std::atomic<bool> _isTerminating;
};

ClientSession::ClientSession(ChatServer& server, boost::asio::io_service &ioService) :
  _server(server),
  _socket(ioService),
  _nameValid(false),
  _state(ALL_RUNNING) { }


void ClientSession::start() {
  _readerThread = std::thread(std::bind(&ClientSession::readerThread, this));
  _writerThread = std::thread(std::bind(&ClientSession::writerThread, this));
}

void ClientSession::sendMessage(const std::shared_ptr<std::string>& msg) {
  std::lock_guard<std::mutex> guard(_messagesMutex);
  _messages.push_back(msg);
  _writerCondition.notify_one();
}

void ClientSession::waitToFinish() {
  int state = _state.load();
  assert(state & READER_TERMINATED);
  assert(state & WRITER_TERMINATED);

  _readerThread.join();
  _writerThread.join();
}

static const char str[] = "What's your name?\n";

void ClientSession::readerThread() {
  try {
    bool loginSuccessfull = false;
    while (! loginSuccessfull && _state == ALL_RUNNING) {
      boost::asio::write(_socket, boost::asio::buffer(str, sizeof(str)-1));
      std::string name = readLineFromClient();
      if ((loginSuccessfull = _server.setClientName(shared_from_this(), name))) {
	std::string response = "Welcome to the chat, " + name + "!\n";
	boost::asio::write(_socket, boost::asio::buffer(response));
      }
      else {
	std::string response = "Name '" + name + "' is already taken, invent another one.\n";
	boost::asio::write(_socket, boost::asio::buffer(response));
      }
    }

    while (parseLine(readLineFromClient()));
  }
  catch (std::exception& ex) {
    const std::string *name = getName();
    std::string formattedName = name ? "'" + *name + "'" : "(null)";
    std::cout << "Client " << formattedName << " reader thread exeption: " << ex.what() << std::endl;
  }

  onReaderShutdown();
}

std::string ClientSession::readLineFromClient() {
  boost::asio::read_until(_socket, _inputBuffer, '\n');
  std::istream stream(&_inputBuffer);
  std::string line;
  std::getline(stream, line);
  return line;
}

bool ClientSession::parseLine(const std::string& line) {
  if (line == "/quit") {
    return false;
  }
  else if (line == "/shutdown") {
    _server.shutdown();
    return false;
  }
  else {
    std::ostringstream stream;
    stream << boost::posix_time::microsec_clock::local_time() << ' '
	   << _name << ": " << line << std::endl;
    _server.broadcast(*this, std::make_shared<std::string>(stream.str()));
    return true;
  }
}

void ClientSession::onReaderShutdown() {
  int oldValue = _state.fetch_or(READER_TERMINATED);
  if (oldValue & WRITER_TERMINATED) {
    _server.removeClient(shared_from_this());
  }
  else {
    _writerCondition.notify_one();
  }
}    

void ClientSession::writerThread() {
  try {
    bool run = true;
    while (run) {
      auto msg = getMessage();
      if (msg) {
	boost::asio::write(_socket, boost::asio::buffer(*msg));
      }
      else {
	run = false;
      }
    }
  }
  catch (std::exception& ex) {
    const std::string *name = getName();
    std::string formattedName = name ? "'" + *name + "'" : "(null)";
    std::cout << "Client " << formattedName << " writer thread exeption: " << ex.what() << std::endl;
  }

  onWriterShutdown();
}

std::shared_ptr<std::string> ClientSession::getMessage() {
  std::unique_lock<std::mutex> lock(_messagesMutex);
  _writerCondition.wait(lock, [this]() { return _state != ALL_RUNNING || ! _messages.empty(); });
  if (_state != ALL_RUNNING) {
    return std::shared_ptr<std::string>();
  }
  else {
    auto msg = _messages.front();
    _messages.pop_front();
    return msg;
  }
}

void ClientSession::onWriterShutdown() {
  int oldValue = _state.fetch_or(WRITER_TERMINATED);
  if (oldValue & READER_TERMINATED) {
    _server.removeClient(shared_from_this());
  }
  else {
    interruptReader();
  }
}

void ClientSession::interruptReader() {
  _state.fetch_or(READER_TERMINATION_REQUESTED);
  pthread_kill(_readerThread.native_handle(), SIGUSR1);
}

void ClientSession::terminate() {
  interruptReader();
}

ChatServer::ChatServer(int port) :
  _acceptor(_ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
  _reaperThread(std::bind(&ChatServer::reaperThread, this)),
  _isTerminating(false) { }


ChatServer::~ChatServer() {
  _reaperThread.join();
}

void ChatServer::run() {
  _acceptingThreadId = pthread_self();
  while (! _isTerminating) {
    std::shared_ptr<ClientSession> client = std::make_shared<ClientSession>(*this, _ioService);
    _acceptor.accept(client->socket());
    std::lock_guard<std::mutex> guard(_clientsMutex);
    if (! _isTerminating) {
      auto p = _clients.insert(client);
      assert(p.second);
      client->start();
    }
  }
}

bool ChatServer::setClientName(const std::shared_ptr<ClientSession>& client,
			       const std::string &name) {
  std::lock_guard<std::mutex> guard(_namesToClientsMutex);
  NamesToClientsMap::iterator found = _namesToClients.find(&name);
  if (found == _namesToClients.end()) {
    client->setName(name);
    auto res = _namesToClients.insert(std::make_pair(client->getName(), client));
    assert(res.second);
    return true;
  }
  else {
    return false;
  }
}

void ChatServer::broadcast(ClientSession& sender,
			   const std::shared_ptr<std::string>& msg) {
  std::vector<std::shared_ptr<ClientSession> > clients;
  {
    std::lock_guard<std::mutex> guard(_namesToClientsMutex);
    clients.reserve(_namesToClients.size());
    for (const auto& kvPair : _namesToClients) {
      clients.push_back(kvPair.second);
    }
  }
  for (const auto& receiver : clients) {
    if (receiver.get() != &sender) {
      receiver->sendMessage(msg);
    }
  }
}

void ChatServer::removeClient(std::shared_ptr<ClientSession>&& client) {
  std::lock_guard<std::mutex> guard(_clientsToRemoveMutex);
  _clientsToRemove.push_back(std::move(client));
  _reaperCondition.notify_one();
}

void ChatServer::reaperThread() {
  try {
    while (true) {
      std::shared_ptr<ClientSession> client = getClientToRemove();
      client->waitToFinish();
      {
	std::lock_guard<std::mutex> guard(_namesToClientsMutex);
	_namesToClients.erase(client->getName());
      }
      {
	std::lock_guard<std::mutex> guard(_clientsMutex);
	size_t res = _clients.erase(client);
	assert(res == 1);
	if (_isTerminating && _clients.empty()) {
	  break;
	}
      }	
    }
  }
  catch (std::exception& ex) {
    std::cout << "Reaper thread exception: " << ex.what() << std::endl;
  }
}

std::shared_ptr<ClientSession> ChatServer::getClientToRemove() {
  std::unique_lock<std::mutex> lock(_clientsToRemoveMutex);
  _reaperCondition.wait(lock, [this]() { return ! _clientsToRemove.empty(); });
  std::shared_ptr<ClientSession> client = _clientsToRemove.front();
  _clientsToRemove.pop_front();
  return client;
}

void ChatServer::shutdown() {
  std::unique_lock<std::mutex> lock(_clientsMutex);
  for (const auto& client : _clients) {
    client->terminate();
  }
  _isTerminating = true;
  lock.unlock();
  pthread_kill(_acceptingThreadId, SIGUSR1);
}

static void handler(int) { }

int main(int argc, char **argv) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = handler;
  if (sigaction(SIGUSR1, &action, nullptr) < 0) {
    perror("sigaction");
    return 1;
  }
  
  try {
    if (argc < 2) {
      std::cerr << "Usage: " << argv[0] << " <port>\n";
      return 1;
    }
    ChatServer server(boost::lexical_cast<int>(argv[1]));
    server.run();
    return 0;
  }
  catch (std::exception &ex) {
    std::cerr << "Main thread exception: " << ex.what() << std::endl;
    return 1;
  }
}
