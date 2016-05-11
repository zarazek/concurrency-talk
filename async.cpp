#include <boost/asio.hpp>
#include <boost/date_time.hpp>
#include <boost/lexical_cast.hpp>

#include <array>
#include <deque>
#include <map>

#include <functional>

using boost::asio::ip::tcp;
using namespace std::placeholders;

class ChatServer;
class ReadHandler;
class WriteHandler;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  ClientSession(ChatServer& server, boost::asio::io_service &ioService) :
    _server(server),
    _socket(ioService),
    _nameValid(false),
    _sendingAllowed(false) { }

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

  void start() {
    askForUserName();
  }
 
  void sendMessage(const std::shared_ptr<std::string>& msg);

private:
  void askForUserName();
  void readUserName();
  void handleUserName(const std::string& userName);
  void startReceivingAndSendingMessages();
  void messageOutputFinished();
  void handleInputLine(const std::string& line);
  bool parseLine(const std::string& line);

  void asyncReadLine(void (ClientSession::*handler)(const std::string& line));
  template <class Buffer>
  void asyncWrite(const Buffer& buffer, void (ClientSession::*handler)());

  void handleReadError(const boost::system::error_code& error);
  std::string readLineFromClient();
  void handleWriteError(const boost::system::error_code& error);
  void terminate();

  ChatServer& _server;
  tcp::socket _socket;
  std::string _name;
  bool _nameValid;
  boost::asio::streambuf _inputBuffer;
  std::string _outputBuffer;
  std::deque<std::shared_ptr<std::string> > _messages;
  bool _sendingAllowed;

  friend class ReadHandler;
  friend class WriteHandler;
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

  bool setClientName(const std::shared_ptr<ClientSession>& client, const std::string& userName);
  void broadcast(ClientSession& client, const std::shared_ptr<std::string>& msg);
  void removeClient(ClientSession& client);
  void shutdown();

private:
  void startAccept();
  void onAccept(std::shared_ptr<ClientSession> client, const boost::system::error_code& error);

  boost::asio::io_service _ioService;
  tcp::acceptor _acceptor;
  NamesToClientsMap _namesToClients;
};


class ReadHandler {
public:
  ReadHandler(void (ClientSession::*handler)(const std::string&),
	      const std::shared_ptr<ClientSession>& client) :
    _handler(handler),
    _client(client) { }

  ReadHandler(void (ClientSession::*handler)(const std::string&),
	      std::shared_ptr<ClientSession>&& client) :
    _handler(handler),
    _client(client) { }

  void operator()(const boost::system::error_code& error, size_t) {
    if (error) {
      _client->handleReadError(error);
      return;
    }
    std::string line = _client->readLineFromClient();
    ((*_client).*_handler)(line);
  }

private:
  void (ClientSession::*_handler)(const std::string&);
  std::shared_ptr<ClientSession> _client;
};

class WriteHandler {
public:
  WriteHandler(void (ClientSession::*handler)(), const std::shared_ptr<ClientSession>& client) :
    _handler(handler),
    _client(client) { }

  WriteHandler(void (ClientSession::*handler)(), std::shared_ptr<ClientSession>&& client) :
    _handler(handler),
    _client(client) { }

  void operator()(const boost::system::error_code& error, size_t) {
    if (error) {
      _client->handleWriteError(error);
      return;
    }
    ((*_client).*_handler)();
  }

private:
  void (ClientSession::*_handler)();
  std::shared_ptr<ClientSession> _client;
};

static const char str[] = "What's your name?\n";

void ClientSession::askForUserName() {
  asyncWrite(boost::asio::buffer(str, sizeof(str)-1), &ClientSession::readUserName);
}

void ClientSession::readUserName() {
  asyncReadLine(&ClientSession::handleUserName);
}

void ClientSession::handleUserName(const std::string& userName) {
  if (_server.setClientName(shared_from_this(), userName)) {
    _outputBuffer = "Welcome to the chat, " + userName + "!\n";
    asyncWrite(boost::asio::buffer(_outputBuffer), &ClientSession::startReceivingAndSendingMessages);
  }
  else {
    _outputBuffer = "Name '" + userName + "' is already taken, invent another one.\n";
    asyncWrite(boost::asio::buffer(_outputBuffer), &ClientSession::askForUserName);
  }
}

void ClientSession::startReceivingAndSendingMessages() {
  _sendingAllowed = true;
  if (! _messages.empty()) {
    asyncWrite(boost::asio::buffer(*_messages.front()), &ClientSession::messageOutputFinished);
  }
  asyncReadLine(&ClientSession::handleInputLine);
}
  

void ClientSession::handleInputLine(const std::string& line) {
  if (parseLine(line)) {
    asyncReadLine(&ClientSession::handleInputLine);
  }
  else {
    terminate();
  }
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
    stream << boost::posix_time::microsec_clock::local_time() << ' ' << _name << ": " << line << std::endl;
    _server.broadcast(*this, std::make_shared<std::string>(stream.str()));
    return true;
  }
}

void ClientSession::messageOutputFinished() {
  assert(_sendingAllowed);
  assert(! _messages.empty());
  _messages.pop_front();
  if (! _messages.empty()) {
    asyncWrite(boost::asio::buffer(*_messages.front()), &ClientSession::messageOutputFinished);
  }
}

void ClientSession::sendMessage(const std::shared_ptr<std::string> &msg) {
  bool startSending = _messages.empty() && _sendingAllowed;
  _messages.push_back(msg);
  if (startSending) {
    asyncWrite(boost::asio::buffer(*_messages.front()), &ClientSession::messageOutputFinished);
  }
}

void ClientSession::asyncReadLine(void (ClientSession::*handler)(const std::string&)) {
  boost::asio::async_read_until(_socket, _inputBuffer, '\n',
				ReadHandler(handler, shared_from_this()));
}

template <class Buffer>
void ClientSession::asyncWrite(const Buffer& buffer, void (ClientSession::*handler)()) {
  boost::asio::async_write(_socket, buffer, WriteHandler(handler, shared_from_this()));
}
							     
std::string ClientSession::readLineFromClient() {
  std::istream stream(&_inputBuffer);
  std::string line;
  std::getline(stream, line);
  return line;
}

void ClientSession::handleReadError(const boost::system::error_code& error) {
  std::cout << "Client reading error: " << error.message() << std::endl;
  terminate();
}

void ClientSession::handleWriteError(const boost::system::error_code& error) {
  std::cout << "Client writing error: " << error.message() << std::endl;
  terminate();
}

void ClientSession::terminate() {
  _socket.cancel();
  _server.removeClient(*this);
}

void ChatServer::run() {
  startAccept();
  _ioService.run();
}

void ChatServer::startAccept() {
  std::shared_ptr<ClientSession> client = std::make_shared<ClientSession>(*this, _ioService);
  _acceptor.async_accept(client->socket(),
			 std::bind(&ChatServer::onAccept, this, client, _1));
}

void ChatServer::onAccept(std::shared_ptr<ClientSession> client,
			  const boost::system::error_code& error) {
  if (! error) {
    client->start();
  }
  else {
    std::cout << "Accept error: " << error << std::endl;
  }

  startAccept();
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

void ChatServer::broadcast(ClientSession& client, const std::shared_ptr<std::string>& msg) {
  for (const auto& kvPair : _namesToClients) {
    ClientSession* receiver = kvPair.second.get();
    if (receiver!= &client) {
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

    boost::asio::io_service ioService;

    
    ChatServer server(boost::lexical_cast<int>(argv[1]));
    server.run();
    return 0;
  }
  catch (std::exception &ex) {
    std::cerr << "Main thread exception: " << ex.what() << std::endl;
    return 1;
  }
}
