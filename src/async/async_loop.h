#ifndef ASYNC_ASYNC_LOOP_H
#define ASYNC_ASYNC_LOOP_H

#include <functional>
#include <memory>
#include <cstddef>

class async_loop;

class async_fd {
public:
  enum mode {
    none,
    read,
    write,
    readwrite
  };
  virtual int fd() const = 0;
  virtual bool error() const = 0;
  virtual void stop() = 0;
  virtual bool ready_read() const = 0;
  virtual bool ready_write() const = 0;
  virtual void set_mode(const mode&) =  0;
  virtual async_loop & loop() = 0;
};

typedef std::function<void(async_fd&)> fd_cb_fn_t;
typedef std::function<void(async_loop&)> timer_cb_fn_t;

class async_loop {
public:
  virtual size_t id() const = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void signal() = 0;
  virtual void add_fd(const int fd, const async_fd::mode mode,
                      fd_cb_fn_t fd_cb_fn) = 0;
  virtual void remove_fd(const int fd) = 0;
  virtual void set_fd_mode(const int fd, const async_fd::mode mode) = 0;
};

typedef std::function<void(async_loop&)> async_cb_fn_t;

class async_events_interface {
public:
  virtual void start_loop(size_t loop_id) = 0;
  virtual void signal_loop(size_t loop_id) = 0;
  virtual void stop_all_loops() = 0;
  virtual async_loop & loop(size_t loop_id) = 0;
};

class async_events : public async_events_interface {
public:
  async_events(size_t, async_cb_fn_t);
  async_events(size_t, async_cb_fn_t, const float, timer_cb_fn_t);
  void start_loop(size_t loop_id);
  void signal_loop(size_t loop_id);
  void stop_all_loops();
  async_loop & loop(size_t loop_id);

private:
  std::unique_ptr<async_events_interface> impl_;
};

typedef std::function<void(const int fd)> on_new_client_fn_t;
typedef std::function<void(const int signal)> on_signal_fn_t;

class main_async_loop_interface {
public:
  virtual void start() = 0;
  virtual void add_tcp_listen_fd(const int fd, on_new_client_fn_t fn) = 0;
};

class main_async_loop : public main_async_loop_interface {
public:
  main_async_loop(main_async_loop_interface & impl);
  void start();
  void add_tcp_listen_fd(const int fd, on_new_client_fn_t fn);

private:
  main_async_loop_interface & impl_;
};

extern main_async_loop g_main_loop;

#endif