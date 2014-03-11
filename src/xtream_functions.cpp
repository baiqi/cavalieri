#include <glog/logging.h>
#include <boost/optional.hpp>
#include <algorithm>
#include <queue>
#include <atomic>
#include <atom.h>
#include <util.h>
#include <scheduler/scheduler.h>
#include <xtream_functions.h>

namespace {
  const unsigned int k_default_ttl = 60;
}

xtream_node_t  prn() {
  return create_xtream_node(
    [](forward_fn_t, e_t e)
    {
      LOG(INFO) << "prn() " <<  event_to_json(e);
    });
}

xtream_node_t with(const with_changes_t & changes, const bool & replace)
{
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e)
    {
      Event ne(e);

      for (auto & kv: changes) {
        set_event_value(ne, kv.first, kv.second, replace);
      }

      forward(ne);
    });
}

xtream_node_t with(const with_changes_t& changes) {
  return with(changes, true);
}

xtream_node_t default_to(const with_changes_t& changes)
{
  return with(changes, false);
}

xtream_node_t split(const split_clauses_t clauses, xtream_node_t default_stream)
{
  return create_xtream_node(
    [=](forward_fn_t, e_t e) {

      for (auto const & pair: clauses) {

        if (pair.first(e)) {
          push_event(pair.second, e);
          return;
        }

      }

      if (default_stream) {
        push_event(default_stream, e);
      }
  });
}

xtream_node_t where(const predicate_t & predicate)
{
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

      if (predicate(e)) {
        forward(e);
      }

  });
}

xtream_node_t where(const predicate_t & predicate,
                   xtream_node_t else_xtream)
{
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

      if (predicate(e)) {
        forward(e);
      } else {
        push_event(else_xtream, e);
      }

  });
}

typedef std::unordered_map<std::string, xtream_node_t> by_stream_map_t;

xtream_node_t by(const by_keys_t & keys, const by_stream_t stream) {

  auto atom_streams = make_shared_atom<by_stream_map_t>();

  return create_xtream_node(

    [=](forward_fn_t, e_t e) mutable {

      if (keys.empty()) {
        return;
      }

      std::string key;
      for (const auto & k: keys) {
        key += string_to_value(e, k) + " ";
      }

      map_on_sync_insert<std::string, xtream_node_t>(
          atom_streams,
          key,
          [&]() { return stream();},
          [&](xtream_node_t  c) { push_event(c, e); }
      );

  });
}

xtream_node_t rate(const int interval) {

  auto rate = std::make_shared<std::atomic<double>>(0);
  bool task_created = false;

  return create_xtream_node(

    [=](forward_fn_t forward, e_t e) mutable
    {

      if (!task_created) {

        g_scheduler.add_periodic_task(
          [=]() mutable
          {
            VLOG(3) << "rate-timer()";

            Event event;

            event.set_metric_d(rate->exchange(0) / interval);
            event.set_time(g_scheduler.unix_time());

            forward(event);
          },

          interval
        );

          task_created = true;

      }

      double expected, newval;

      do {

        expected = rate->load();
        newval = expected + metric_to_double(e);

      } while (!rate->compare_exchange_strong(expected, newval));

    }

  );

}

typedef std::unordered_map<std::string, Event> coalesce_events_t;

xtream_node_t coalesce(fold_fn_t fold) {

  auto coalesce = make_shared_atom<coalesce_events_t>();

  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) mutable
    {

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

          forward(fold(expired_events));

          events_t events;
          for (const auto & it : curr) {
            events.push_back(it.second);
          }

          forward(fold(events));
        }
      );
    }
  );
}

typedef std::vector<boost::optional<Event>> project_events_t;

xtream_node_t project(const predicates_t predicates, fold_fn_t fold) {

  auto events = make_shared_atom<project_events_t>({predicates.size(),
                                                    boost::none});
  return create_xtream_node(

    [=](forward_fn_t forward, e_t e) mutable {

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
          forward(fold(expired_events));

          events_t events;
          for (const auto & ev : curr) {
            if (ev) {
              events.push_back(*ev);
            }
          }

          if (!events.empty()) {
            forward(fold(events));
          }
        }

      );
    }
  );
}


xtream_node_t changed_state(std::string initial) {
  auto prev = make_shared_atom<std::string>(initial);

  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

      prev->update(
        e.state(),
        [&](const std::string & prev, const std::string &)
        {
          if (prev != e.state()) {
            forward(e);
          }
        }
      );

    });
}

xtream_node_t tagged_any(const tags_t& tags) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e)
    {

      if (tagged_any_(e, tags)) {
        forward(e);
      }

    });
}

xtream_node_t tagged_all(const tags_t& tags) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e)
    {
      if (tagged_all_(e, tags)) {
        forward(e);
      }

    });
}

xtream_node_t smap(smap_fn_t f) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e)
    {
      Event ne(e);

      f(ne);

      forward(ne);
    });
}

xtream_node_t moving_event_window(size_t n, fold_fn_t fold) {

  auto window = make_shared_atom<std::list<Event>>();

  return create_xtream_node(

    [=](forward_fn_t forward, e_t e) {

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

          forward(fold({begin(curr), end(curr)}));

        }

      );
    });
}

xtream_node_t fixed_event_window(size_t n, fold_fn_t fold) {

  auto window = make_shared_atom<std::list<Event>>();

  return create_xtream_node(

    [=](forward_fn_t forward, e_t e) {

      std::list<Event> event_list;
      bool forward_events;

      window->update(

        [&](const std::list<Event> w) -> std::list<Event>
        {

          event_list = conj(w, e);

          if ((forward_events = (event_list.size() == n))) {
            return {};
          } else {
            return event_list;
          }

        },

        [&](const std::list<Event> &, const std::list<Event> &) {

          if (forward_events) {
            forward(fold({begin(event_list), end(event_list)}));
          }

        }
      );
    }
  );
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

xtream_node_t moving_time_window(time_t dt, fold_fn_t fold) {

  auto window = make_shared_atom<moving_time_window_t>();

  return create_xtream_node(

    [=](forward_fn_t forward, e_t e) {

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

           forward(fold(events));
        }
      );
  });
}

typedef struct {
  std::priority_queue<Event, std::vector<Event>, event_time_cmp> pq;
  time_t start{0};
  time_t max{0};
  bool started{false};
} fixed_time_window_t;

xtream_node_t fixed_time_window(time_t dt, fold_fn_t fold) {

  auto window = make_shared_atom<fixed_time_window_t>();

  return create_xtream_node(

    [=](forward_fn_t forward, e_t e) {

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
           forward(fold(flush)); 
        }
      );
  });
}


typedef struct {
  std::string state;
  std::vector<Event> buffer;
  time_t start{0};
} stable_t;

xtream_node_t stable(time_t dt) {

  auto stable = make_shared_atom<stable_t>();

  return create_xtream_node(

    [=](forward_fn_t forward, e_t e)
    {

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
            for (const auto & flush_event: flush) {
              forward(flush_event);
            }
          }
      );
    }
  );

}

typedef struct {
  size_t forwarded{0};
 time_t new_interval{0};
} throttle_t;

xtream_node_t throttle(size_t n, time_t dt) {

  auto throttled = make_shared_atom<throttle_t>();

  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) mutable {

      bool forward_event;

      throttled->update(
        [&](const throttle_t & t) {

          auto c(t);

          if (c.new_interval < e.time()) {
            c.new_interval += e.time() + dt;
            c.forwarded = 0;
          }

          forward_event = (c.forwarded < n);

          if (forward_event) {
            c.forwarded++;
          }

          return c;
        },

        [&](const throttle_t &, const throttle_t &) {

          if (forward_event) {
            forward(e);
          }

      });
    }
  );

}

xtream_node_t above(double m) {
  return create_xtream_node(
   [=](forward_fn_t forward, e_t e) {

      if (above_(e,m)) {
        forward(e);
      }

  });
}

xtream_node_t under(double m) {
  return create_xtream_node(
   [=](forward_fn_t forward, e_t e) {

      if (under_(e,m)) {
        forward(e);
      }

  });
}

xtream_node_t within(double a, double b) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

    if (above_eq_(e,a) && under_eq_(e, b)) {
      forward(e);
    }

  });
}

xtream_node_t without(double a, double b) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

    if (under_(e,a) || above_(e, b)) {
      forward(e);
    }

  });
}

xtream_node_t scale(double s) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

      Event ne(e);

      double t = s * metric_to_double(e);

      clear_metrics(ne);
      ne.set_metric_d(t);

      forward(ne);
  });
}

xtream_node_t sdo() {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {
      forward(e);
    });
}

xtream_node_t counter() {
   auto counter = std::make_shared<std::atomic<unsigned int>>(0);

  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) mutable {

      if (metric_set(e)) {

        Event ne(e);
        ne.set_metric_sint64(counter->fetch_add(1) + 1);
        forward(ne);

      } else {

        forward(e);

      }
  });
}

xtream_node_t expired() {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {
      if (expired_(e)) {
        forward(e);
      }
  });
}

xtream_node_t tag(tags_t tags) {
  return create_xtream_node(
    [=](forward_fn_t forward, e_t e) {

      Event ne(e);

      for (const auto & t: tags) {
        *(ne.add_tags()) = t;
      }

      forward(ne);
    }
  );
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

predicate_t state_pred(const std::string state) {
  return PRED(e.state() == state);
}

predicate_t service_pred(const std::string service) {
  return PRED(e.service() == service);
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
  auto ttl = e.has_ttl() ? e.ttl() : k_default_ttl;

  if (e.state() == "expired") {
    return true;
  }

  if (g_scheduler.unix_time() < e.time()) {
    return false;
  }

  return (g_scheduler.unix_time() - e.time() > ttl);
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

void streams::add_stream(xtream_node_t stream) {
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
    ::push_event(s, e);
  }
}

