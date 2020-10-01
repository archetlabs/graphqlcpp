#pragma once
#ifndef GRAPHQL_HELPER_HPP
#define GRAPHQL_HELPER_HPP

#include "./session.hpp"
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <tuple>

namespace archet {
namespace graphql {
namespace helper {

std::tuple<std::shared_ptr<graphql::session>, std::thread>
make_session(std::string host, std::string port, std::string endpoint, std::string authorization) {
  std::shared_ptr<graphql::session> graphql(nullptr);
  std::mutex creation_mutex;
  creation_mutex.lock();

  std::thread session_thread([&creation_mutex, &graphql, host, port, endpoint, authorization] {
    net::io_context ioc;
    nlohmann::json headers = {
        {"authorization", authorization},
    };

    ssl::context ctx{ssl::context::tlsv13_client};
    ctx.set_verify_mode(ssl::verify_none);

    auto internal = std::make_shared<graphql::session>(ioc, ctx);
    internal->connect(host, port, endpoint, headers);

    graphql = internal;
    creation_mutex.unlock();
    ioc.run();

    assert(false);
    // we should never reach here
    std::cerr << "catastrophic failure" << std::endl;
  });
  creation_mutex.lock();

  return {graphql, std::move(session_thread)};
}

} // namespace helper
} // namespace graphql
} // namespace archet
#endif
