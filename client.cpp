#include "client.h"
#include <cassert>
#include <sys/epoll.h>

client::client(int fd, proxy_server *proxyServer) : socket(fd), request_server(nullptr),
                                                    event(proxyServer->get_epoll(), get_fd(), EPOLLIN,
                                                          [proxyServer, this](uint32_t events) mutable throw(std::runtime_error) {
                                                              try {

                                                                  if (events & EPOLLIN) {
                                                                      std::cout << "EPOLLIN in client\n";
                                                                      this->read_request(proxyServer);
                                                                  }
                                                                  if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                                                                      disconnect(proxyServer);
                                                                      return;
                                                                  }
                                                                  if (events & EPOLLOUT) {
                                                                      std::cout << "Client EPOLLOUT\n";
                                                                      write_response(proxyServer);
                                                                  }

                                                              } catch (...) {
                                                                  std::cout << "Client error\n";
                                                                  disconnect(proxyServer);
                                                              }

                                                          }), time(proxyServer->get_epoll().get_time_service(), SOCKET_TIMEOUT, [proxyServer, this]() {
            std::cout << "Timeout client\n";
            this->disconnect(proxyServer);

        }) {

};

client::~client() {
    if (request_server) {
        unbind();
    }
    if (request != nullptr) {
        request.reset();
    }
    if (response != nullptr) {
        response.reset();
    }
}

file_descriptor &client::get_fd() {
    return socket.get_fd();
}

file_descriptor &client::get_server_fd() {
    assert(request_server);
    return request_server->get_fd();
}

bool client::has_server() {
    return request_server != nullptr;
}

std::string &client::get_buffer() {
    return buffer;
}

size_t client::get_buffer_size() {
    return buffer.size();
}

std::string client::get_host() {
    assert(request_server);
    return request_server->get_host();
}

size_t client::read() {
    try {
        std::string reads = socket.read(socket.get_available_bytes());
        buffer.append(reads);
        return reads.length();
    } catch (...) {
        return 0;
    }
}

size_t client::write() {
    try {
        size_t written_cnt = socket.write(buffer);
        buffer.erase(0, written_cnt);
        if (request_server) {
            flush_server_buffer();
        }
        return written_cnt;
    } catch (...) {
        return 0;
    }
}

void client::bind(class server *new_server) {
    request_server.reset(new_server);
    request_server->bind(this);
}

void client::unbind() {
    request_server.reset(nullptr);
}

void client::flush_client_buffer() {
    assert(request_server);
    request_server->append(buffer);
    buffer.clear();
}

void client::flush_server_buffer() {
    assert(request_server);
    request_server->push_to_client();
}

void client::set_response(class http_response *rsp) {
    response.reset(rsp);
}

class http_response *client::get_response() {
    return response.get();
}

void client::set_request(class http_request *rsp) {
    request.reset(rsp);
}

class http_request *client::get_request() {
    return request.get();
}

void client::read_request(proxy_server *proxyServer) {
    if (socket.get_available_bytes() == 0){
        event.remove_flag(EPOLLIN);
        return;
    }
    time.change_time(SOCKET_TIMEOUT);
    size_t read_cnt = read();
    fprintf(stdout, "Read data from client, fd = %lu,size = %zu\n", get_fd().get_fd(), read_cnt);

    std::unique_ptr<http_request> cur_request(new http_request(get_buffer()));

    if (cur_request->get_stat() == http_request::BAD){
        buffer = http_protocol::BAD_REQUEST();
        event.remove_flag(EPOLLIN);
        event.add_flag(EPOLLOUT);
        return;
    }

    if (cur_request->is_ended()) {
        fprintf(stdout, "Request for host [%s]\n", cur_request->get_host().c_str());

        http_response *response = new http_response();
        set_response(response);

        set_request(new http_request(*cur_request.get()));

        if (has_server()) {
            if (get_host() == cur_request->get_host()) {
                buffer = cur_request->get_data();
                flush_client_buffer();
                request_server->add_flag(EPOLLOUT);
                return;
            } else {
                proxyServer->erase_server(get_server_fd().get_fd());
                unbind();
            }
        }

        buffer = cur_request->get_data();
        cur_request->set_client_fd(get_fd().get_fd());
        proxyServer->add_task(std::move(cur_request));
    }
}

void client::write_response(proxy_server *proxyServer) {
    fprintf(stdout, "Writing data to client, fd = %lu\n", get_fd().get_fd());
    time.change_time(SOCKET_TIMEOUT);
    write();
    if (has_server()) {
        flush_server_buffer();
    }
    if (get_buffer_size() == 0) {
        event.remove_flag(EPOLLOUT);
    }
}


void client::disconnect(proxy_server *proxyServer) {
    fprintf(stdout, "Disconnect client, fd = %lu\n", get_fd().get_fd());

    event.remove_flag(EPOLLIN);
    event.remove_flag(EPOLLOUT);

    if (has_server()) {
        fprintf(stdout, "Disconnect server, and client fd = %d\n", get_server_fd().get_fd());
        proxyServer->erase_server(get_server_fd().get_fd());
    }

    proxyServer->erase_client(get_fd().get_fd());
}
