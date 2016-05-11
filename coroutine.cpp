#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/date_time.hpp>
#include <boost/lexical_cast.hpp>

#include <deque>
#include <map>

#include <functional>

using boost::asio::ip::tcp;
using namespace std::placeholders;

class ConditionVariable {
public:
  ConditionVariable(boost::asio::io_service &ioService);

  template <class Predicate>
  void wait(boost::asio::yield_context yield, Predicate pred);

  void notify_one();
  void notify_all();
private:
  boost::asio::deadline_timer _timer;
};

ConditionVariable::ConditionVariable(boost::asio::io_service& ioService) :
  _timer(ioService) { }

template <class Predicate>
void ConditionVariable::wait(boost::asio::yield_context yield, Predicate pred) {
  boost::system::error_code ec;
  while (! pred()) {
    _timer.async_wait(yield[ec]);
    if (ec == boost::asio::error::operation_aborted) {
      return;
    }
    else {
      throw boost::system::system_error(ec);
    }
  }
}

void ConditionVariable::notify_one() {
  _timer.cancel_one();
}

void ConditionVariable::notify_all() {
  _timer.cancel();
}

class ChatServer;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  ClientSession(ChatServer& server, boost::asio::io_service &ioService) :
    _server(server),
    _ioService(ioService),
    _socket(ioService),
    _nameValid(false),
    _writerCondition(ioService),
    _state(ALL_RUNNING) { }

  tcp::socket &socket() {
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
  
private:
  void readerThread(boost::asio::yield_context yield);
  std::string readLineFromClient(boost::asio::yield_context yield);
  bool parseLine(const std::string& line);
  void writerThread(boost::asio::yield_context yield);
  std::shared_ptr<std::string> getMessage(boost::asio::yield_context yield);

  template <class Buffer>
  void asyncWrite(const Buffer& buffer, boost::asio::yield_context yield);
  

  enum {
    ALL_RUNNING = 0,
    READER_TERMINATED = 1,
    WRITER_TERMINATED = 2,
    TERMINATE_REQUESTED = 4
  };

  void onReaderShutdown();
  void onWriterShutdown();

  ChatServer& _server;
  boost::asio::io_service& _ioService;
  tcp::socket _socket;
  std::string _name;
  bool _nameValid;
  boost::asio::streambuf _inputBuffer;
  ConditionVariable _writerCondition;
  std::deque<std::shared_ptr<std::string> > _outputData;
  int _state;
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

typedef std::map<const std::string*, std::shared_ptr<ClientSession>, PtrLess<std::string> > NamesToClientsMap;

class ChatServer {
public:
  ChatServer(int port) :
    _acceptor(_ioService, tcp::endpoint(tcp::v4(), port)) { }

  void run();

  bool setClientName(const std::shared_ptr<ClientSession>& client, const std::string &name);
  void broadcast(ClientSession& sender, const std::shared_ptr<std::string>& msg);
  void removeClient(ClientSession& client);
  void shutdown();
private:
  void acceptThread(boost::asio::yield_context yield);

  boost::asio::io_service _ioService;
  tcp::acceptor _acceptor;
  NamesToClientsMap _namesToClients;
};

void ClientSession::start() {
  boost::asio::spawn(_ioService, std::bind(&ClientSession::readerThread, shared_from_this(), _1));
  boost::asio::spawn(_ioService, std::bind(&ClientSession::writerThread, shared_from_this(), _1));
}

void ClientSession::sendMessage(const std::shared_ptr<std::string>& msg) {
  _outputData.push_back(msg);
  _writerCondition.notify_one();
}

static const char str[] = "What's your name?\n";

void ClientSession::readerThread(boost::asio::yield_context yield) {
  try {
    bool loginSuccessfull = false;
    while (! loginSuccessfull) {
      asyncWrite(boost::asio::buffer(str, sizeof(str)-1), yield);
      std::string name = readLineFromClient(yield);
      if ((loginSuccessfull = _server.setClientName(shared_from_this(), name))) {
	std::string response = "Welcome to the chat, " + name + "!\n";
	asyncWrite(boost::asio::buffer(response), yield);
      }
      else {
	std::string response = "Name '" + name + "' is already taken, invent another one.\n";
	asyncWrite(boost::asio::buffer(response), yield);
      }
    }

    while (parseLine(readLineFromClient(yield)));
  }
  catch (std::exception& ex) {
    std::cout << "Client reader thread exeption: " << ex.what() << std::endl;
  }

  onReaderShutdown();
}

std::string ClientSession::readLineFromClient(boost::asio::yield_context yield) {
  boost::system::error_code ec;
  boost::asio::async_read_until(_socket, _inputBuffer, '\n', yield[ec]);
  if (ec) {
    throw boost::system::system_error(ec);
  }
  std::istream stream(&_inputBuffer);
  std::string line;
  std::getline(stream, line);
  return line;
}

bool ClientSession::parseLine(const std::string& line) {
  if (line == "/quit") {
    return false;
  }
  if (line == "/shutdown") {
    _server.shutdown();
    return false;
  }
  else {
    std::ostringstream stream;
    stream << boost::posix_time::microsec_clock::local_time() << ' ' << _name << " > " << line << std::endl;
    _server.broadcast(*this, std::make_shared<std::string>(stream.str()));
    return true;
  }
}

void ClientSession::onReaderShutdown() {
  int oldState = _state;
  _state = oldState | READER_TERMINATED;
  if (oldState & WRITER_TERMINATED) {
    _server.removeClient(*this);
  }
  else {
    _writerCondition.notify_one();
  }
}

void ClientSession::writerThread(boost::asio::yield_context yield) {
  try {
    boost::system::error_code ec;
    bool run = true;
    while (run) {
      auto msg = getMessage(yield);
      if (msg) {
	asyncWrite(boost::asio::buffer(*msg), yield);
      }
      else {
	run = false;
      }
    }
  }
  catch (std::exception& ex) {
    std::cout << "Client writer thread exception: " << ex.what() << std::endl;
  }

  onWriterShutdown();
}

std::shared_ptr<std::string> ClientSession::getMessage(boost::asio::yield_context yield) {
  auto self = shared_from_this();
  _writerCondition.wait(yield, [self]() { return ! (self->_state == ALL_RUNNING &&
						    self->_outputData.empty()); });
  if (_state == ALL_RUNNING) {
    auto msg = _outputData.front();
    _outputData.pop_front();
    return msg;
  }
  else {
    return std::shared_ptr<std::string>();
  }
}

void ClientSession::onWriterShutdown() {
  int oldState = _state;
  _state = oldState | WRITER_TERMINATED;
  if (oldState & READER_TERMINATED) {
    _server.removeClient(*this);
  }
  else {
    _socket.cancel();
  }
}

template <class Buffer>
void ClientSession::asyncWrite(const Buffer& buffer, boost::asio::yield_context yield) {
  boost::system::error_code ec;
  boost::asio::async_write(_socket, buffer, yield[ec]);
  if (ec) {
    throw boost::system::system_error(ec);
  }
}

void ClientSession::terminate() {
  _state |= TERMINATE_REQUESTED;
  _socket.cancel();
  _writerCondition.notify_all();
}

void ChatServer::run() {
  boost::asio::spawn(_ioService,  std::bind(&ChatServer::acceptThread, this, _1));
  _ioService.run();
}

void ChatServer::acceptThread(boost::asio::yield_context yield) {
  boost::system::error_code ec;
  while (true) {
    std::shared_ptr<ClientSession> client = std::make_shared<ClientSession>(*this, _ioService);
    _acceptor.async_accept(client->socket(), yield[ec]);
    if (ec) {
      throw boost::system::system_error(ec);
    }
    client->start();
  }
}

bool ChatServer::setClientName(const std::shared_ptr<ClientSession>& client,
			       const std::string &name) {
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
  for (const auto& kvPair : _namesToClients) {
    ClientSession* receiver = kvPair.second.get();
    if (receiver != &sender) {
      receiver->sendMessage(msg);
    }
  }
}

void ChatServer::removeClient(ClientSession& client) {
  _namesToClients.erase(client.getName());
}

void ChatServer::shutdown() {
  _ioService.stop();
}

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      std::cerr << "Usage: " << argv[0] << " <port>\n";
      return 1;
    }
    std::unique_ptr<ChatServer> server(new ChatServer(boost::lexical_cast<int>(argv[1])));
    server->run();
    return 0;
  }
  catch (std::exception &ex) {
    std::cerr << "Main thread exception: " << ex.what() << std::endl;
    return 1;
  }
}
