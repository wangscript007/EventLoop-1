#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <set>
#include <map>

#include "eventloop.h"

using std::set;
using std::map;

namespace eventloop {

class SignalManager {
 public:
  int AddEvent(BaseSignalEvent *e);
  int DeleteEvent(BaseSignalEvent *e);
  int UpdateEvent(BaseSignalEvent *e);

 private:
  map<int, set<BaseSignalEvent *> > sig_events_;

 public:
  static SignalManager *Instance() {
    if (!instance_) {
      instance_ = new SignalManager();
    }
    return instance_;
  }
 private:
  SignalManager();
 private:
  static SignalManager *instance_;
};

SignalManager *SignalManager::instance_ = NULL;

class TimerManager {
 public:
  int AddEvent(BaseTimerEvent *e);
  int DeleteEvent(BaseTimerEvent *e);
  int UpdateEvent(BaseTimerEvent *e);

 private:
  friend class EventLoop;
  class Compare {
   public:
    bool operator()(const BaseTimerEvent *e1, const BaseTimerEvent *e2) {
      timeval t1 = e1->GetTime();
      timeval t2 = e2->GetTime();
      return (t1.tv_sec < t2.tv_sec) || (t1.tv_sec == t2.tv_sec && t1.tv_usec < t2.tv_usec);
    }
  };

  typedef set<BaseTimerEvent *, Compare> TimerSet;

 private:
  TimerSet timers_;
};

int SetNonblocking(int fd) {
  int opts;
  if ((opts = fcntl(fd, F_GETFL)) != -1) {
    opts = opts | O_NONBLOCK;
    if(fcntl(fd, F_SETFL, opts) != -1) {
      return 0;
    }
  }
  return -1;
}

int ConnectTo(const char *host, short port) {
  int fd;
  struct sockaddr_in addr;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_family = PF_INET;
  addr.sin_port = htons(port);
  if (host[0] == '\0' || strcmp(host, "localhost") == 0) {
    inet_aton("127.0.0.1", &addr.sin_addr);
  } else if (!strcmp(host, "any")) {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else {
    inet_aton(host, &addr.sin_addr);
  }

  SetNonblocking(fd);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1) {
    if (errno != EINPROGRESS) {
      return -1;
    }
  }

  return fd;
}

int BindTo(const char *host, short port) {
  int fd, on = 1;
  struct sockaddr_in addr;

  if ((fd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
    return -1;
  }

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = PF_INET;
  addr.sin_port = htons(port);
  if (host[0] == '\0' || strcmp(host, "localhost") == 0) {
    inet_aton("127.0.0.1", &addr.sin_addr);
  } else if (strcmp(host, "any") == 0) {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else {
    if (inet_aton(host, &addr.sin_addr) == 0) return -1;
  }

  if (bind(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1 || listen(fd, 10) == -1) {
    return -1;
  }

  return fd;
}

// TimerManager implementation
int TimerManager::AddEvent(BaseTimerEvent *e) {
  return !timers_.insert(e).second;
}

int TimerManager::DeleteEvent(BaseTimerEvent *e) {
  return timers_.erase(e) != 1;
}

int TimerManager::UpdateEvent(BaseTimerEvent *e) {
  timers_.erase(e);
  return !timers_.insert(e).second;
}

// EventLoop implementation
EventLoop::EventLoop() {
  epfd_ = epoll_create(256);
  timermanager_ = new TimerManager();
}

EventLoop::~EventLoop() {
  close(epfd_);
  static_cast<TimerManager *>(timermanager_)->timers_.clear();
  delete static_cast<TimerManager *>(timermanager_);
}

int EventLoop::GetFileEvents(int timeout) {
  return epoll_wait(epfd_, evs_, 256, timeout);
}

int EventLoop::DoTimeout() {
  int n = 0;
  TimerManager::TimerSet& timers = static_cast<TimerManager *>(timermanager_)->timers_;
  TimerManager::TimerSet::iterator ite = timers.begin();
  while (ite != timers.end()) {
    timeval tv = (*ite)->GetTime();
    if ((tv.tv_sec > now_.tv_sec) || (tv.tv_sec == now_.tv_sec && tv.tv_usec > now_.tv_usec)) break;
    n++;
    BaseTimerEvent *e = *ite;
    timers.erase(ite);
    e->Process(BaseTimerEvent::TIMER);
    ite = timers.begin();
  }
  return n;
}

int EventLoop::ProcessEvents(int timeout) {
  int i, nt, n = GetFileEvents(timeout);

  gettimeofday(&now_, NULL);

  nt = DoTimeout();

  for(i = 0; i < n; i++) {
    BaseEvent *e = (BaseEvent *)evs_[i].data.ptr;
    uint32_t events = 0;
    if (evs_[i].events & EPOLLIN) events |= BaseFileEvent::READ;
    if (evs_[i].events & EPOLLOUT) events |= BaseFileEvent::WRITE;
    if (evs_[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) events |= BaseFileEvent::ERROR;
    e->Process(events);
  }

  return nt + n;
}

void EventLoop::StopLoop() {
  stop_ = true;
}

void EventLoop::StartLoop() {
  stop_ = false;
  while (!stop_) {
    int timeout = 100;
    if (static_cast<TimerManager *>(timermanager_)->timers_.size() > 0) {
      TimerManager::TimerSet::iterator ite = static_cast<TimerManager *>(timermanager_)->timers_.begin();
      timeval tv = (*ite)->GetTime();
      int t = tv.tv_sec * 1000 + tv.tv_usec / 1000;
      if (timeout > t) timeout = t;
    }
    ProcessEvents(timeout);
  }
}

int EventLoop::AddEvent(BaseFileEvent *e) {
  struct epoll_event ev;

  uint32_t type = e->GetType();
  ev.events = 0;
  if (type | BaseFileEvent::READ) ev.events |= EPOLLIN;
  if (type | BaseFileEvent::WRITE) ev.events |= EPOLLOUT;
  if (type | BaseFileEvent::ERROR) ev.events |= EPOLLHUP | EPOLLERR | EPOLLRDHUP;
  ev.data.fd = e->GetFile();
  ev.data.ptr = e;

  SetNonblocking(e->GetFile());

  return epoll_ctl(epfd_, EPOLL_CTL_ADD, e->GetFile(), &ev);
}

int EventLoop::UpdateEvent(BaseFileEvent *e) {
  struct epoll_event ev;
  uint32_t type = e->GetType();

  ev.events = 0;
  if (type | BaseFileEvent::READ) ev.events |= EPOLLIN;
  if (type | BaseFileEvent::WRITE) ev.events |= EPOLLOUT;
  if (type | BaseFileEvent::ERROR) ev.events |= EPOLLHUP | EPOLLERR | EPOLLRDHUP;
  ev.data.fd = e->GetFile();
  ev.data.ptr = e;

  return epoll_ctl(epfd_, EPOLL_CTL_MOD, e->GetFile(), &ev);
}

int EventLoop::DeleteEvent(BaseFileEvent *e) {
  struct epoll_event ev; //kernel before 2.6.9 requires
  return epoll_ctl(epfd_, EPOLL_CTL_DEL, e->GetFile(), &ev);
}

int EventLoop::AddEvent(BaseTimerEvent *e) {
  return static_cast<TimerManager *>(timermanager_)->AddEvent(e);
}

int EventLoop::UpdateEvent(BaseTimerEvent *e) {
  return static_cast<TimerManager *>(timermanager_)->UpdateEvent(e);
}

int EventLoop::DeleteEvent(BaseTimerEvent *e) {
  return static_cast<TimerManager *>(timermanager_)->DeleteEvent(e);
}

static void sig_handler(int signo) {
}

int SignalManager::AddEvent(BaseSignalEvent *e) {
  struct sigaction action;
  action.sa_handler = sig_handler;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);

  if (e->GetType() & BaseSignalEvent::INT) {
    sig_events_[SIGINT].insert(e);
    sigaction(SIGINT, &action, NULL);
  }

  if (e->GetType() & BaseSignalEvent::PIPE) {
    sig_events_[SIGPIPE].insert(e);
    sigaction(SIGPIPE, &action, NULL);
  }

  if (e->GetType() & BaseSignalEvent::TERM) {
    sig_events_[SIGTERM].insert(e);
    sigaction(SIGTERM, &action, NULL);
  }

  return 0;
}

int SignalManager::DeleteEvent(BaseSignalEvent *e) {
  return 0;
}

int SignalManager::UpdateEvent(BaseSignalEvent *e) {
  return 0;
}

}
