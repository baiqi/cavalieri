#include <unordered_map>
#include <algorithm>
#include <queue>
#include <memory>
#include <atomic>
#include <boost/optional.hpp>
#include <glog/logging.h>
#include <streams.h>
#include <util.h>
#include <scheduler.h>
#include <atom.h>

namespace {
  const unsigned int default_ttl = 60;
  // TODO: use function template
  std::vector<stream_t> children_to_vec(const children_t & children) {
    if (children.which() == 0) {
      return {1, boost::get<stream_t>(children)};
    } else {
      return boost::get<std::vector<stream_t>>(children);
    }
  }
  std::vector<mstream_t> children_to_vec(const mchildren_t & children) {
    if (children.which() == 0) {
      return {1, boost::get<mstream_t>(children)};
    } else {
      return boost::get<std::vector<mstream_t>>(children);
    }
  }
};

void call_rescue(e_t e, const children_t& children) {
  for (auto& s: children_to_vec(children)) {
    s(e);
  }
}

void call_rescue(const std::vector<Event> events, const children_t& children) {
  for (auto& e: events) {
    for (auto& s: children_to_vec(children)) {
      s(e);
    }
  }
}

void call_rescue(const std::list<Event> events, const children_t& children) {
  for (auto& e: events) {
    for (auto& s: children_to_vec(children)) {
      s(e);
    }
  }
}

void call_rescue(const std::vector<Event> events, const mchildren_t& children) {
  if (!events.empty()) {
    for (auto& s: children_to_vec(children)) {
      s(events);
    }
  }
}

void call_rescue(const std::list<Event> events, const mchildren_t& children) {
  if (!events.empty()) {
    for (const auto& s: children_to_vec(children)) {
      s(std::vector<Event>{begin(events), end(events)});
    }
  }
}

stream_t prn() {
  return [](e_t e) {
    LOG(INFO) << "prn() " <<  event_to_json(e);
  };
}

stream_t with(const with_changes_t & changes,
              const bool & replace,
              const children_t & children)
{
  return [=](e_t e) {
    Event ne(e);
    for (auto &kv: changes) {
      set_event_value(ne, kv.first, kv.second, replace);
    }
    call_rescue(ne, children);
  };
}

stream_t with(const with_changes_t& changes, const children_t& children) {
  return with(changes, true, children);
}

stream_t with_ifempty(
    const with_changes_t& changes,
    const children_t& children)
{
  return with(changes, false, children);
}

stream_t split(const split_clauses_t clauses,
               const stream_t& default_stream)
{
  return [=](e_t e) {
    for (auto const &pair: clauses) {
      if (pair.first(e)) {
        call_rescue(e, {pair.second});
        return;
      }
    }
    if (default_stream) {
      call_rescue(e, {default_stream});
    }
  };
}

typedef std::unordered_map<std::string, const children_t> by_streams_map_t;

stream_t by(const by_keys_t & keys, const by_streams_t & streams) {
  auto atom_streams = make_shared_atom<by_streams_map_t>();

  return [=](e_t e) mutable {
    if (keys.empty()) {
      return;
    }

    std::string key;
    for (const auto & k: keys) {
      key += string_to_value(e, k) + " ";
    }

    auto fn_value = [&]() {
      std::vector<stream_t> children;
      for (auto & s: streams) {
        children.push_back(s());
      }
      return children;
    };

    map_on_sync_insert<std::string, const children_t>(
        atom_streams,
        key,
        fn_value,
        [&](const children_t & c) { call_rescue(e, c); }
    );
  };
}

stream_t where(const predicate_t& predicate, const children_t& children,
               const children_t& else_children)
{
  return [=](e_t e) {
    if (predicate(e)) {
      call_rescue(e, children);
    } else {
      call_rescue(e, else_children);
    }
  };
}

stream_t rate(const int interval, const children_t& children) {
  auto rate = std::make_shared<std::atomic<double>>(0);
  g_scheduler.add_periodic_task(
     [=]() mutable
      {
        VLOG(3) << "rate-timer()";
        Event e;
        e.set_metric_d(rate->exchange(0) / interval);
        e.set_time(g_scheduler.unix_time());
        VLOG(3) << "rate-timer() value: " << e.metric_d();
        call_rescue(e, children);
      },
      interval
  );

  return [=](e_t e) mutable {
    double expected, newval;
    do {
      expected = rate->load();
      newval = expected + metric_to_double(e);
    } while (!rate->compare_exchange_strong(expected, newval));
  };
}

typedef std::unordered_map<std::string, Event> coalesce_events_t;

stream_t coalesce(const mchildren_t & children) {
  auto coalesce = make_shared_atom<coalesce_events_t>();
  return [=](e_t e) mutable {
    std::vector<Event> expired_events;
    coalesce->update(
        [&](const coalesce_events_t & events) {
          coalesce_events_t c;
          std::string key(e.host() + " " +  e.service());
          expired_events.clear();
          c[key] = e;
          for (const auto & it : events) {
            if (key == it.first) {
              continue;
            }
            if (expired_(it.second)) {
              expired_events.push_back(it.second);
            } else {
              c.insert({it.first, it.second});
            }
          }
          return c;
        },
        [&](const coalesce_events_t &, const coalesce_events_t & curr) {
          call_rescue(expired_events, children);
          events_t events;
          for (const auto & it : curr) {
            events.push_back(it.second);
          }
          call_rescue(events, children);
        }
    );
  };
}

typedef std::vector<boost::optional<Event>> project_events_t;

stream_t project(const predicates_t predicates, const mchildren_t& children) {
  auto events = make_shared_atom<project_events_t>({predicates.size(),
                                                    boost::none});
  return [=](e_t e) mutable {
    std::vector<Event> expired_events;
    events->update(
        [&](const project_events_t & curr) {
          auto c(curr);
          expired_events.clear();
          bool match = false;
          for (size_t i = 0; i < predicates.size(); i++) {
            if (!match && predicates[i](e)) {
              c[i] = e;
              match = true;
            } else {
              if (c[i] && expired_(*c[i])) {
                expired_events.push_back(*c[i]);
                c[i].reset();
              }
            }
          }
          return c;
        },
        [&](const project_events_t &, const project_events_t & curr) {
          call_rescue(expired_events, children);
          events_t events;
          for (const auto & ev : curr) {
            if (ev) {
              events.push_back(*ev);
            }
          }
          if (!events.empty()) {
            call_rescue(events, children);
          }
        }
    );
  };
}

stream_t changed_state(std::string initial, const children_t& children) {
  auto prev = make_shared_atom<std::string>(initial);

  return [=](e_t e) mutable {
    prev->update(
        e.state(),
        [&](const std::string & prev, const std::string &)
          {
            if (prev != e.state())
              call_rescue(e, children);
          }
    );
  };
}

stream_t tagged_any(const tags_t& tags, const children_t& children) {
  return [=](e_t e) {
    if (tagged_any_(e, tags)) {
      call_rescue(e, children);
    }
  };
}

stream_t tagged_all(const tags_t& tags, const children_t& children) {
  return [=](e_t e) {
    if (tagged_all_(e, tags)) {
      call_rescue(e, children);
    }
  };
}

stream_t expired(const children_t & children) {
  return [=](e_t e) {
    if (expired_(e)) {
      call_rescue(e, children);
    }
  };
}

bool tagged_any_(e_t e, const tags_t& tags) {
  for (auto &tag: tags) {
    if (tag_exists(e, tag)) {
      return true;
    }
  }
  return false;
}

bool tagged_all_(e_t e, const tags_t& tags) {
  for (auto &tag: tags) {
    if (!tag_exists(e, tag)) {
      return false;
    }
  }
  return true;
}

bool expired_(e_t e) {
  auto ttl = e.has_ttl() ? e.ttl() : default_ttl;

  if (e.state() == "expired") {
    return true;
  }

  if (g_scheduler.unix_time() < e.time()) {
    return false;
  }

  return (g_scheduler.unix_time() - e.time() > ttl);
}

stream_t send_index(class index& idx) {
  return [&](e_t e) {
    if (e.state() != "expired") {
      idx.add_event(e);
    }
  };
}

stream_t smap(smap_fn_t f, const children_t& children) {
  return [=](e_t e) {
    Event ne(e);
    f(ne);
    call_rescue(ne, children);
  };
}


stream_t moving_event_window(size_t n, const mchildren_t& children) {
  auto window = make_shared_atom<std::list<Event>>();

  return [=](e_t e) {
    window->update(
      [&](const std::list<Event> w)
        {
          auto c(w);
          c.push_back(e);
          if (c.size() == (n + 1)) {
            c.pop_front();
          }
          return std::move(c);
        },
      [&](const std::list<Event> &, const std::list<Event> & curr) {
        call_rescue(curr, children);
      }
    );
  };
}

stream_t fixed_event_window(size_t n, const mchildren_t& children) {
  auto window = make_shared_atom<std::list<Event>>();

  return [=](e_t e) {
    std::list<Event> event_list;
    bool forward;
    window->update(
      [&](const std::list<Event> w) -> std::list<Event>
        {
          event_list = conj(w, e);
          if ((forward = (event_list.size() == n))) {
            return {};
          } else {
            return event_list;
          }
        },
      [&](const std::list<Event> &, const std::list<Event> &) {
        if (forward) {
          call_rescue(event_list, children);
        }
      }
    );
  };
}

struct event_time_cmp
{
  bool operator() (const Event & lhs, const Event & rhs) const
  {
    return (lhs.time() > rhs.time());
  }
};

typedef struct {
  std::priority_queue<Event, std::vector<Event>, event_time_cmp> pq;
  time_t max{0};
} moving_time_window_t;

stream_t moving_time_window(time_t dt, const mchildren_t& children) {
  auto window = make_shared_atom<moving_time_window_t>();

  return [=](e_t e) {
    window->update(
      [&](const moving_time_window_t  w )
        {
          auto c(w);

          if (!e.has_time()) {
            return c;
          }

          if (e.time() > w.max) {
            c.max = e.time();
          }

          c.pq.push(e);

          if (c.max < dt) {
            return c;
          }

          while (!c.pq.empty() && c.pq.top().time() <= (c.max - dt)) {
            c.pq.pop();
          }

          return c;
        },
      [&](const moving_time_window_t &, const moving_time_window_t & curr) {
        auto c(curr);
        std::vector<Event> events;
        while (!c.pq.empty()) {
          events.push_back(c.pq.top());
          c.pq.pop();
        }
        call_rescue(events, children);
      }
    );
  };
}

typedef struct {
  std::priority_queue<Event, std::vector<Event>, event_time_cmp> pq;
  time_t start{0};
  time_t max{0};
  bool started{false};
} fixed_time_window_t;

stream_t fixed_time_window(time_t dt, const mchildren_t& children) {
  auto window = make_shared_atom<fixed_time_window_t>();

  return [=](e_t e) {
    std::vector<Event> flush;
    window->update(
      [&](const fixed_time_window_t  w)
        {
          auto c(w);
          flush.clear();

          // Ignore event with no time
          if (!e.has_time()) {
            return c;
          }

          if (!c.started) {
            c.started = true;
            c.start = e.time();
            c.pq.push(e);
            c.max = e.time();
            return c;
          }

          // Too old
          if (e.time() < c.start) {
            return c;
          }

          if (e.time() > c.max) {
            c.max = e.time();
          }

          time_t next_interval = c.start - (c.start % dt) + dt;

          c.pq.push(e);

          if (c.max < next_interval) {
            return c;
          }

          // We can flush a window
          while (!c.pq.empty() && c.pq.top().time() < next_interval) {
            flush.emplace_back(c.pq.top());
            c.pq.pop();
          }

          c.start = next_interval;

          return c;
        },
      [&](const fixed_time_window_t &, const fixed_time_window_t &) {
          call_rescue(flush, children);
      }
    );
  };
}

typedef struct {
  std::string state;
  std::vector<Event> buffer;
  time_t start{0};
} stable_t;

typedef std::shared_ptr<atom<stable_t>> atom_stable_t;

void try_flush_stable(
    atom_stable_t stable,
    time_t original_start,
    std::string original_state,
    children_t children)
{
  std::vector<Event> flush;
  bool abort = false;
  stable->update(
      [&](const stable_t & s) {
        auto c(s);

        if (abort) {
          return c;
        }

        if (c.buffer.empty() ||
            (c.start != original_start) ||
            (c.state != original_state))
        {
          abort = true;
          return c;
        }

        flush = std::move(c.buffer);
        return c;
      },
      [&](const stable_t &, const stable_t) {
        call_rescue(flush, children);
      }
  );
}

stream_t stable(time_t dt, const children_t& children) {
  auto stable = make_shared_atom<stable_t>();

  return [=](e_t e) {
    std::vector<Event> flush;
    bool schedule_flush;
    stable->update(
        [&](const stable_t & s) {
          auto c(s);
          flush.clear();
          schedule_flush = false;

          if (s.state != e.state()) {
            c.start = e.time();
            c.buffer.clear();
            c.buffer.push_back(e);
            c.state = e.state();
            schedule_flush = true;
            return c;
          }

          if (e.time() < c.start) {
            return c;
          }

          if (c.start + dt > e.time()) {
            c.buffer.push_back(e);
            return c;
          }

          if (!c.buffer.empty()) {
            flush = std::move(c.buffer);
          }
          flush.push_back(e);

          return c;
        },
        [&](const stable_t &, const stable_t &) {
          call_rescue(flush, children);
        }
    );
  };
}

typedef struct {
  size_t forwarded{0};
 time_t new_interval{0};
} throttle_t;

stream_t throttle(size_t n, time_t dt, const children_t & children) {
  auto throttled = make_shared_atom<throttle_t>();

  return [=](e_t e) mutable {
    bool forward;
    throttled->update(
        [&](const throttle_t & t) {
          auto c(t);
          if (c.new_interval < e.time()) {
            c.new_interval += e.time() + dt;
            c.forwarded = 0;
          }
          forward = (c.forwarded < n);
          if (forward) {
            c.forwarded++;
          }
          return c;
        },
        [&](const throttle_t &, const throttle_t &) {
          if (forward) {
            call_rescue(e, children);
          }
    });
  };
}

stream_t above(double m, const children_t & children) {
  return [=](e_t e) {
    if (above_(e,m)) {
      call_rescue(e, children);
    }
  };
}

stream_t under(double m, const children_t & children) {
  return [=](e_t e) {
    if (under_(e,m)) {
      call_rescue(e, children);
    }
  };
}

stream_t within(double a, double b, const children_t & children) {
  return [=](e_t e) {
    if (above_eq_(e,a) && under_eq_(e, b)) {
      call_rescue(e, children);
    }
  };
}

stream_t without(double a, double b, const children_t & children) {
  return [=](e_t e) {
    if (under_(e,a) || above_(e,b)) {
      call_rescue(e, children);
    }
  };
}

stream_t scale(double s, const children_t & children) {
  return [=](e_t e) {
      Event ne(e);

      double t = s * metric_to_double(e);

      clear_metrics(ne);
      ne.set_metric_d(t);

      call_rescue(ne, children);
  };
}

stream_t counter(const children_t & children) {
 auto counter = std::make_shared<std::atomic<unsigned int>>(0);

  return [=](e_t e) mutable {
    if (metric_set(e)) {
      Event ne(e);
      ne.set_metric_sint64(counter->fetch_add(1) + 1);
      call_rescue(ne, children);
    } else {
      call_rescue(e, children);
    }
  };
}

stream_t tag(tags_t tags, const children_t& children) {
  return [=](e_t e) {
    Event ne(e);
    for (const auto & t: tags) {
      *(ne.add_tags()) = t;
    }
    call_rescue(ne, children);
  };
}

predicate_t above_eq_pred(const double value) {
  return PRED(above_eq_(e, value));
}

predicate_t above_pred(const double value) {
  return PRED(above_(e, value));
}

predicate_t under_eq_pred(const double value) {
  return PRED(under_eq_(e, value));
}

predicate_t under_pred(const double value) {
  return PRED(under_(e, value));
}

bool above_eq_(e_t e, const double value) {
  return (metric_to_double(e) >= value);
}

bool above_(e_t e, const double value) {
  return (metric_to_double(e) > value);
}

bool under_eq_(e_t e, const double value) {
  return (metric_to_double(e) <= value);
}

bool under_(e_t e, const double value) {
  return (metric_to_double(e) < value);
}


void streams::add_stream(stream_t stream) {
  VLOG(3) << "adding stream";
  streams_.push_back(stream);
}

void streams::process_message(const Msg& message) {
  VLOG(3) << "process message. num of streams " << streams_.size();
  unsigned long int sec = time(0);
  for (int i = 0; i < message.events_size(); i++) {
    const Event &event = message.events(i);
    if (!event.has_time()) {
      Event nevent(event);
      nevent.set_time(sec);
      push_event(std::move(nevent));
    } else {
      push_event(event);
    }
  }
}

void streams::push_event(const Event& e) {
  for (auto& s: streams_) {
    s(e);
  }
}


