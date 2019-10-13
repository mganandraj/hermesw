#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "ws_session.h"

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

// Report a failure
void fail(boost::system::error_code ec, char const* what)
{
  std::cerr << what << ": " << ec.message() << "\n";
}


class ws_session : public std::enable_shared_from_this<ws_session>, public web_socket_session_interface
{
  tcp::resolver resolver_;
  websocket::stream<tcp::socket> ws_;
  boost::beast::flat_buffer buffer_;
  std::string host_;

  std::function<void(std::string)> read_func_;
  std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func_;

  std::mutex write_queue_mutex_;
  std::vector<std::string> write_queue_;
  std::condition_variable write_queue_cv;

  std::thread write_thread;
public:
  // client
  explicit ws_session(boost::asio::io_context& ioc, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func) : resolver_(ioc), ws_(ioc), on_connected_func_(on_connected_func) {}

  // server
  // Take ownership of the socket
  explicit ws_session(boost::asio::io_context& ioc, tcp::socket socket, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func) : resolver_(ioc), ws_(std::move(socket)) {}

  void write(std::string text) override {
    std::lock_guard<std::mutex> lk(write_queue_mutex_);
    write_queue_.emplace_back(text);
    write_queue_cv.notify_all();
  }

  void writer_thread_func() {
    while (true) {
      std::unique_lock<std::mutex> lk(write_queue_mutex_);
      write_queue_cv.wait(lk, [this] { return !write_queue_.empty(); });

      std::vector<std::string> write_queue_copy = std::move(write_queue_);
      lk.unlock();

      for (std::string text : write_queue_copy)
      {
        ws_.write(boost::asio::buffer(text));
      }
    }
  }

  void setOnRead(std::function<void(std::string)> func) override {
    read_func_ = func;
  }

  void close() {
    ws_.async_close(websocket::close_code::normal, std::bind(&ws_session::on_close, shared_from_this(), std::placeholders::_1));
  }

  void on_close(boost::system::error_code ec) {
    if (ec)
      return fail(ec, "close");
  }

  void run_server()
  {
    ws_.async_accept(boost::beast::bind_front_handler(&ws_session::on_accept, shared_from_this()));
  }

  void on_accept(boost::system::error_code ec)
  {
    if (ec)
      return fail(ec, "accept");

    write_thread = std::thread(&ws_session::writer_thread_func, this);

    // Read a message
    do_read();
  }

  // Start the asynchronous operation
  void run_client(char const* host, char const* port)
  {
    // Save these for later
    host_ = host;

    // Look up the domain name
    resolver_.async_resolve(host, port, std::bind(&ws_session::on_resolve, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
  }

  void on_resolve(boost::system::error_code ec, tcp::resolver::results_type results) {
    if (ec)
      return fail(ec, "resolve");

    // Make the connection on the IP address we get from a lookup
    boost::asio::async_connect(ws_.next_layer(), results.begin(), results.end(), std::bind(&ws_session::on_connect, shared_from_this(), std::placeholders::_1));
  }

  void on_connect(boost::system::error_code ec) {
    if (ec)
      return fail(ec, "connect");

    // Perform the websocket handshake
    ws_.async_handshake(host_, "/", std::bind(&ws_session::on_handshake, shared_from_this(), std::placeholders::_1));
  }

  void on_handshake(boost::system::error_code ec) {
    if (ec)
      return fail(ec, "handshake");

    on_connected_func_(shared_from_this());

    write_thread = std::thread(&ws_session::writer_thread_func, this);

    // Read a message
    do_read();
  }

  void do_read()
  {
    // Read a message into our buffer
    ws_.async_read(buffer_, boost::beast::bind_front_handler(&ws_session::on_read, shared_from_this()));
  }

  void on_read(boost::system::error_code ec, std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    // This indicates that the session was closed
    if (ec == websocket::error::closed)
      return;

    if (ec)
      fail(ec, "read");

    ws_.text(ws_.got_text());

    std::string text = boost::beast::buffers_to_string(buffer_.data());
    if (read_func_ && !text.empty()) {
      read_func_(text);
    }

    // Clear the buffer
    buffer_.consume(buffer_.size());

    // Read a message
    do_read();
  }
};

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func_;
  boost::asio::io_context& ioc_;

public:
  listener(boost::asio::io_context& ioc, tcp::endpoint endpoint, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func)
    : ioc_(ioc), acceptor_(ioc), socket_(ioc), on_connected_func_(on_connected_func)
  {
    boost::system::error_code ec;

    // Open the acceptor
    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
    {
      fail(ec, "open");
      return;
    }

    // Bind to the server address
    acceptor_.bind(endpoint, ec);
    if (ec)
    {
      fail(ec, "bind");
      return;
    }

    // Start listening for connections
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec)
    {
      fail(ec, "listen");
      return;
    }
  }

  // Start accepting incoming connections
  void run()
  {
    if (!acceptor_.is_open())
      return;

    do_accept();
  }

  void do_accept()
  {
    acceptor_.async_accept(socket_, std::bind(&listener::on_accept, shared_from_this(), std::placeholders::_1));
  }

  void on_accept(boost::system::error_code ec)
  {
    if (ec)
    {
      fail(ec, "accept");
    }
    else
    {
      // Create the session and run it
      auto session = std::make_shared<ws_session>(ioc_, std::move(socket_), on_connected_func_);
      on_connected_func_(session);
      session->run_server();
    }

    // Accept another connection
    do_accept();
  }
};

void create_web_socket_server(unsigned short port, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func) {
  boost::asio::io_context ioc;
  std::make_shared<listener>(ioc, tcp::endpoint{ boost::asio::ip::make_address("0.0.0.0"), port }, on_connected_func)->run();
  ioc.run();
}

void create_web_socket_client(unsigned short port, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func) {
  boost::asio::io_context ioc;
  std::make_shared<ws_session>(ioc, on_connected_func)->run_client("127.0.0.1", "8888");
  ioc.run();
}

void do_server() {
  create_web_socket_server(8888, [](std::shared_ptr<web_socket_session_interface> connection) {
    connection->setOnRead([](std::string text) {
      std::cout << "<< do_server :: Read Handler >> :: " << text << std::endl;
    });

    connection->write("First message from server");
    connection->write("Second message from server");

  });
}

void do_client() {
  create_web_socket_client(8888, [](std::shared_ptr<web_socket_session_interface> connection) {
    connection->setOnRead([](std::string text) {
      std::cout << "<< do_client :: Read Handler >> :: " << text << std::endl;
    });

    connection->write("First message from client");
    connection->write("Second message from client");
  });
}

//int main(int argc, char* argv[])
//{
//
//  std::thread server_thread(do_server);
//
//  // Wait for server to come up.
//  std::this_thread::sleep_for(std::chrono::seconds(1));
//
//  std::thread client_thread(do_client);
//
//  server_thread.join();
//
//  // bool is_client = true;
//
//  //if (argc > 1) {
//  //  // First param must by --client or --server
//  //  if (strcmp(argv[1], "--server") == 0)
//  //    is_client = false;
//  //  else if (strcmp(argv[1], "--client") != 0)
//  //    std::terminate();
//  //}
//
//  //unsigned short port = 8888;
//  //if (argc > 2) {
//  //  if (strcmp(argv[2], "--port") != 0)
//  //    std::terminate(); // Second argument must be port.
//
//  //  if(argc < 4)
//  //    std::terminate(); // port number must be provided.
//
//  //  port = std::stoi(argv[3]);
//  //}
//
//
//  //auto const address = boost::asio::ip::make_address("0.0.0.0");
//  //
//  //boost::asio::io_context ioc;
//
//  /*if (!is_client) {
//    create_web_socket_server(port, )
//  }
//  else {
//    std::make_shared<ws_session>(ioc)->run("127.0.0.1", argv[3], "Hello how are you ?");
//  }*/
//
//  // ioc.run();
//
//  return EXIT_SUCCESS;
//}