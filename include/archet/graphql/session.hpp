#pragma once
#ifndef GRAPHQL_SESSION_HPP
#define GRAPHQL_SESSION_HPP

#include <boost/asio/strand.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>
#include <boost/fiber/future/promise.hpp>
#include <boost/function.hpp>
#include <boost/lambda/lambda.hpp>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <unordered_map>

#define DEFINE_GRAPHQL_OP(name)                                                                    \
  { #name, name }

namespace archet {
namespace graphql {
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

inline std::string to_string(beast::flat_buffer const &buffer) {
  return std::string(boost::asio::buffer_cast<char const *>(beast::buffers_front(buffer.data())),
                     boost::asio::buffer_size(buffer.data()));
}

//------------------------------------------------------------------------------

enum operation_enum {
  ka,
  connection_error,
  ack,
  data,
  complete,
  connection_ack,
  error,
};

std::unordered_map<std::string, operation_enum> operation_label_to_enum = {
    DEFINE_GRAPHQL_OP(ka),       DEFINE_GRAPHQL_OP(connection_error),
    DEFINE_GRAPHQL_OP(ack),      DEFINE_GRAPHQL_OP(data),
    DEFINE_GRAPHQL_OP(complete), DEFINE_GRAPHQL_OP(connection_ack),
    DEFINE_GRAPHQL_OP(error),
};

std::string make_request_message(int op_id, std::string gql, nlohmann::json variables) {
  nlohmann::json j = {{"id", std::to_string(op_id)},
                      {"type", "start"},
                      {"payload",
                       {
                           {"query", gql},
                           {"variables", variables},
                           {"operationName", nlohmann::json()},
                       }}};
  return j.dump();
}

std::string make_stop_message(int op_id) {
  nlohmann::json j = {
      {"id", std::to_string(op_id)},
      {"type", "stop"},
  };
  return j.dump();
}

void fail(beast::error_code ec, char const *what) {
  std::cerr << what << ": " << ec.message() << "\n";
  assert(false);
}

void info(std::string message) {
  std::cerr << "\033[1;37m[info]\033[0;37m " << message << "\033[0m" << std::endl;
}

// Sends a WebSocket message and prints the response
class session : public std::enable_shared_from_this<session> {
  tcp::resolver resolver_;
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
  beast::flat_buffer buffer_;
  std::string host_;
  std::string port_;
  std::string endpoint_;
  nlohmann::json headers_;
  boost::asio::deadline_timer timer_;
  int op_serial_;
  std::unordered_map<int, boost::function<void(nlohmann::json, int)>> results_;
  // boost::lockfree::queue outbox_;
  std::queue<std::string> outbox_;
  boost::asio::io_context::strand strand_;

public:
  explicit session(net::io_context &ioc, ssl::context &ctx)
      : resolver_(net::make_strand(ioc)), ws_(net::make_strand(ioc), ctx), headers_({}),
        timer_(net::make_strand(ioc)), outbox_(), strand_(ioc) {
    op_serial_ = 0;
  }

  void connect(std::string host, std::string port, std::string endpoint, nlohmann::json headers) {
    info("using " + host + ", " + port + ", " + endpoint);
    info("resolving");

    // Save these for later
    host_ = host;
    port_ = port;
    endpoint_ = endpoint;
    headers_ = headers;

    // Look up the domain name
    resolver_.async_resolve(host_, port_,
                            beast::bind_front_handler(&session::on_resolve, shared_from_this()));
  }

  void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec)
      return fail(ec, "resolve");

    info("connecting");

    // Set a timeout on the operation
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

    // Make the connection on the IP address we get from a lookup
    beast::get_lowest_layer(ws_).async_connect(
        results, beast::bind_front_handler(&session::on_connect, shared_from_this()));
  }

  void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec)
      return fail(ec, "connect");

    info("ssl handshaking");

    // Set a timeout on the operation
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

    // Perform the SSL handshake
    ws_.next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&session::on_ssl_handshake, shared_from_this()));
  }

  void on_ssl_handshake(beast::error_code ec) {
    if (ec)
      return fail(ec, "ssl_handshake");

    // Turn off the timeout on the tcp_stream, because
    // the websocket stream has its own timeout system.
    beast::get_lowest_layer(ws_).expires_never();

    // Set suggested timeout settings for the websocket
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

    // Set a decorator to change the User-Agent of the handshake
    ws_.set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
      req.set(http::field::user_agent,
              std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-async-ssl");
    }));

    ws_.async_handshake(
        host_, endpoint_,
        beast::bind_front_handler(&session::on_graphql_handshake, shared_from_this()));
  }

  void on_graphql_handshake(beast::error_code ec) {
    if (ec)
      return fail(ec, "handshake");

    info("graphql handshaking");

    nlohmann::json headers_json = {{"type", "connection_init"},
                                   {"payload", nlohmann::json::object({{"headers", headers_}})}};
    std::string headers = headers_json.dump();
    ws_.async_write(net::buffer(headers),
                    beast::bind_front_handler(&session::start_graphql_handler, shared_from_this()));
  }

  void start_graphql_handler(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    ws_.async_read(buffer_,
                   beast::bind_front_handler(&session::on_graphql_message, shared_from_this()));
  }

  void on_graphql_message(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec)
      return fail(ec, "read");

    try {
      std::string msg_string(to_string(buffer_));
      // std::cerr << "\033[1;34m[debug]\033[0;34m " << msg_string << "\033[0m"
      // << std::endl;
      nlohmann::json msg = nlohmann::json::parse(msg_string);
      std::string type = msg["type"];
      int type_num = operation_label_to_enum[type];
      switch (type_num) {
      case connection_ack:
        timer_.expires_from_now(boost::posix_time::seconds(0));
        break;
      case data: {
        int op_id = std::stoi(msg["id"].get<std::string>());
        timer_.async_wait(strand_.wrap(boost::bind(&session::invoke_handler, shared_from_this(),
                                                   op_id, msg["payload"]["data"])));
        break;
      }
      case error: {
        int op_id = std::stoi(msg["id"].get<std::string>());
        std::cerr << "\033[1;31m[error]\033[0;31m " << msg["payload"]["errors"] << "\033[0m"
                  << std::endl;
        // std::unordered_map<int,
        // boost::function<void(nlohmann::json)>>::const_iterator it =
        // results_.find(op_id); if (it != results_.end()) {
        // boost::function<void(nlohmann::json)> handler = it->second;
        // timer_.async_wait(boost::bind(handler, msg["payload"]["errors"]));
        //}
        break;
      }
      case complete: {
        int op_id = std::stoi(msg["id"].get<std::string>());
        timer_.async_wait(
            strand_.wrap(boost::bind(&session::erase_handler, shared_from_this(), op_id)));
        break;
      }
      }
    } catch (nlohmann::json::exception &e) {
      std::cerr << "message: " << e.what() << '\n' << "exception id: " << e.id << std::endl;
    }

    buffer_.consume(bytes_transferred);

    ws_.async_read(buffer_,
                   beast::bind_front_handler(&session::on_graphql_message, shared_from_this()));
  }

  void on_write(beast::error_code ec, std::size_t bytes_transferred) {
    outbox_.pop();

    boost::ignore_unused(bytes_transferred);

    if (ec)
      return fail(ec, "write");

    if (!outbox_.empty())
      message_outbox_dequeue_();
  }

  void message_outbox_dequeue_(void) {
    std::string request = outbox_.front();
    ws_.async_write(net::buffer(request),
                    beast::bind_front_handler(&session::on_write, shared_from_this()));
  }

  void message_outbox_enqueue_(std::string request) {
    outbox_.push(request);
    if (outbox_.size() > 1) {
      return;
    }

    message_outbox_dequeue_();
  }

  void query_outbox_enqueue_(std::string gql, nlohmann::json variables,
                             boost::function<void(nlohmann::json, int)> handler) {
    int op_id = op_serial_++;
    results_[op_id] = handler; // inside strand_ context
    message_outbox_enqueue_(make_request_message(op_id, gql, variables));
  }

  void erase_handler(int op_id) {
    std::unordered_map<int, boost::function<void(nlohmann::json, int)>>::const_iterator it =
        results_.find(op_id);
    if (it != results_.end()) {
      results_.erase(it);
    }
  }

  void invoke_handler(int op_id, nlohmann::json data) {
    std::unordered_map<int, boost::function<void(nlohmann::json, int)>>::const_iterator it =
        results_.find(op_id);
    if (it != results_.end()) {
      boost::function<void(nlohmann::json, int)> handler = it->second;
      handler(data, op_id);
    }
  }

  void stop_outbox_enqueue_(int op_id) {
    erase_handler(op_id); // inside strand_ context
    message_outbox_enqueue_(make_stop_message(op_id));
  }

  void query(std::string gql, nlohmann::json variables,
             boost::function<void(nlohmann::json, int)> handler) {
    timer_.async_wait(strand_.wrap(
        boost::bind(&session::query_outbox_enqueue_, shared_from_this(), gql, variables, handler)));
  }

  void query(std::string gql, boost::function<void(nlohmann::json, int)> handler) {
    query(gql, nlohmann::json(), handler);
  }

  // only valid when executed in the direct handler execution context
  // which provided the op_id
  void stop(int op_id) {
    erase_handler(op_id); // inside strand_ context
    timer_.async_wait(strand_.wrap(boost::bind(&session::message_outbox_enqueue_,
                                               shared_from_this(), make_stop_message(op_id))));
  }

  // due to delay even after invoked, subscribed function may re-execute
  void stop_without_handler_context(int op_id) {
    timer_.async_wait(
        strand_.wrap(boost::bind(&session::stop_outbox_enqueue_, shared_from_this(), op_id)));
  }

  void on_close(beast::error_code ec) {
    if (ec)
      return fail(ec, "close");

    std::cerr << nlohmann::json::parse(to_string(buffer_)).dump() << std::endl;
  }
};

} // namespace graphql
} // namespace archet
#endif
