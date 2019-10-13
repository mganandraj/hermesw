#pragma once

#include <string>
#include <functional>

struct web_socket_session_interface {
  virtual void write(std::string text) = 0;
  virtual void setOnRead(std::function<void(std::string)>) = 0;
  virtual void close() = 0;
};

void create_web_socket_server(unsigned short port, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func);
void create_web_socket_client(unsigned short port, std::function<void(std::shared_ptr<web_socket_session_interface>)> on_connected_func);