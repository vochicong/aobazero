// 2019 Team AobaZero
// This source code is in the public domain.
#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include "err.hpp"
#include "iobase.hpp"
#include "osi.hpp"
#include "xzi.hpp"
#include <iostream>
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>

using std::min;
using ErrAux::die;
using handler_t = void (*)(int);
using uint = unsigned int;

static void binary2text(char *msg, size_t &len, char &ch_last) noexcept;

#ifdef USE_WINAPI
#  include <atomic>
#  include <mutex>
#  include <thread>
#  include <ws2tcpip.h>
#  include <winsock2.h>
#  include <windows.h>
#  undef max
#  undef min
using std::atomic;
using std::mutex;
using std::lock_guard;
using std::make_shared;
using std::shared_ptr;
using std::thread;

class LastErr {
  enum class Type { API, Sckt };
  void *_msg;

public:
  static constexpr Type api  = Type::API;
  static constexpr Type sckt = Type::Sckt;
  explicit LastErr(Type t = api) noexcept : _msg(nullptr) {
    DWORD dw;
    if (t == sckt) dw = static_cast<DWORD>(WSAGetLastError());
    else           dw = GetLastError();
    if (!FormatMessageA((FORMAT_MESSAGE_ALLOCATE_BUFFER
			 | FORMAT_MESSAGE_FROM_SYSTEM
			 | FORMAT_MESSAGE_IGNORE_INSERTS),
			nullptr, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPSTR) &_msg, 0, nullptr)) _msg = "unknown"; }
  ~LastErr() noexcept { if (_msg) LocalFree(_msg); }
  const char *get() const noexcept { return static_cast<char *>(_msg); } };

class PipeIn_impl {
  thread _t;
  HANDLE _hfile, _do_read, _has_line;
  char _line[65536], _line_out[65536];
  bool _do_getline;
  volatile atomic<bool> _is_eof;
  
  void reader() {
    size_t len_line = 0, len_buf = 0;
    bool eof = false;
    char ch_last = '\0';
    while (true) {
      char buf[BUFSIZ];

      while (len_buf == 0) {

	if (eof) {
	  _is_eof = true;
	  if (!SetEvent(_has_line))
	    die(ERR_INT("SetEvent() failed: %s", LastErr().get()));
	  return; }

	DWORD dw;
	if (!ReadFile(_hfile, buf, sizeof(buf), &dw, NULL)) {
	  if (GetLastError() != ERROR_BROKEN_PIPE)
	    die(ERR_INT("ReadFile() failed: %s", LastErr().get()));
	  eof = true;
	  if (len_line) { buf[0] = '\n'; dw  = 1U; } }
	
	len_buf = dw;
	binary2text(buf, len_buf, ch_last); }
      
      size_t pos = 0;
      for (; pos < len_buf && buf[pos] != '\n'; ++pos) {
	if (sizeof(_line) <= len_line + 1U) die(ERR_INT("buffer overrun"));
	_line[len_line++] = buf[pos]; }
      
      if (pos == len_buf) { len_buf = 0; continue; }
      
      assert(len_line < sizeof(_line));
      pos     += 1U;
      len_buf -= pos;
      memmove(buf, buf + pos, len_buf);
      _line[len_line] = '\0';
      len_line = 0;
      
      if (!ResetEvent(_do_read))
	die(ERR_INT("ResetEvent() failed: %s", LastErr().get()));
      if (!SetEvent(_has_line))
	die(ERR_INT("SetEvent() failed: %s", LastErr().get()));
      if (WaitForSingleObject(_do_read, INFINITE) == WAIT_FAILED)
	die(ERR_INT("WaitForSingleObject() failed: %s", LastErr().get())); } }
  
public:
  explicit PipeIn_impl(HANDLE h) noexcept
    : _hfile(h), _do_read(nullptr), _has_line(nullptr), _do_getline(false),
      _is_eof(false) {
    
    _do_read  = CreateEvent(nullptr, TRUE, TRUE, nullptr);
    if (!_do_read) die(ERR_INT("CreateEvent() failed: %s", LastErr().get()));

    _has_line = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!_has_line) die(ERR_INT("CreateEvent() failed: %s", LastErr().get()));

    _t = thread(&PipeIn_impl::reader, this);
    _t.detach(); }
  
  ~PipeIn_impl() noexcept {
    if (!_is_eof) die(ERR_INT("INTERNAL ERROR"));
    if (!CloseHandle(_hfile)
	|| !CloseHandle(_do_read)
	|| !CloseHandle(_has_line))
      die(ERR_INT("CloseHandle() failed: %s", LastErr().get())); }
  
  char *getline_block() noexcept;

  void load() noexcept {
    if (_is_eof) return;
    DWORD dw = WaitForSingleObject(_has_line, INFINITE);
    if (dw == WAIT_FAILED)
      die(ERR_INT("WaitForSingleObject() failed: %s", LastErr().get()));
    assert(dw == WAIT_OBJECT_0);
    strcpy(_line_out, _line);
    _do_getline = true;
    if (!ResetEvent(_has_line))
      die(ERR_INT("ResetEvent() failed: %s", LastErr().get()));
    if (!SetEvent(_do_read))
      die(ERR_INT("SetEvent() failed: %s", LastErr().get())); }

  char *getline() noexcept {
    if (!_do_getline) return nullptr;
    _do_getline = false;
    return _line_out; }

  bool is_eof() const noexcept { return _is_eof; }
  bool ok() const noexcept {
    return _hfile != nullptr && _do_read != nullptr && _has_line != nullptr; }
  HANDLE get_event() const noexcept { return _has_line; } };

class OSI::Pipe_impl {
public:
  HANDLE out;
  PipeIn_impl in, err;
  char *ready_in, *ready_err;
  bool done_in, done_err;
  explicit Pipe_impl(HANDLE out_, HANDLE in_, HANDLE err_) noexcept
    : out(out_), in(in_), err(err_), done_in(false), done_err(false) {} };

void OSI::Pipe::open(const char *, char * const argv[]) noexcept {
  assert(argv && argv[0]);
  HANDLE h_in_rd  = nullptr, h_in_wr  = nullptr;
  HANDLE h_err_rd = nullptr, h_err_wr = nullptr;
  HANDLE h_out_rd = nullptr, h_out_wr = nullptr;
  SECURITY_ATTRIBUTES sa_attr;
  sa_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa_attr.bInheritHandle = TRUE;
  sa_attr.lpSecurityDescriptor = nullptr;
  if (!CreatePipe(&h_in_rd,  &h_in_wr,  &sa_attr, 0)
      || !CreatePipe(&h_out_rd, &h_out_wr, &sa_attr, 0)
      || !CreatePipe(&h_err_rd, &h_err_wr, &sa_attr, 0))
    die(ERR_INT("CreatePipe() failed: %s", LastErr().get()));
  if (!SetHandleInformation(h_in_wr, HANDLE_FLAG_INHERIT, 0)
      || !SetHandleInformation(h_out_rd, HANDLE_FLAG_INHERIT, 0)
      || !SetHandleInformation(h_err_rd, HANDLE_FLAG_INHERIT, 0))
    die(ERR_INT("SetHandleInformation() failed: %s", LastErr().get()));

  PROCESS_INFORMATION pi; 
  ZeroMemory(&pi, sizeof(pi));
  
  STARTUPINFO si;
  ZeroMemory(&si, sizeof(si));
  si.cb         = sizeof(si); 
  si.hStdError  = h_err_wr;
  si.hStdOutput = h_out_wr;
  si.hStdInput  = h_in_rd;
  si.dwFlags    = STARTF_USESTDHANDLES;

  char line[65536];
  size_t len = 0;
  for (int i = 0; argv[i]; ++i) len += strlen(argv[i]) + 1U;
  if (sizeof(line) < len) die(ERR_INT("command line too long"));
  strcpy(line, argv[0]);
  for (int i = 1; argv[i]; ++i) { strcat(line, " "); strcat(line, argv[i]); }

  if (!CreateProcess(nullptr, line, nullptr, nullptr, TRUE,
		     CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
    die(ERR_INT("CreateProcess() failed: %s", LastErr().get()));
  
  if (!CloseHandle(h_err_wr) || !CloseHandle(h_out_wr)
      || !CloseHandle(h_in_rd))
    die(ERR_INT("CloseHandle() failed: %s", LastErr().get()));

  if (!CloseHandle(pi.hProcess) || !CloseHandle(pi.hThread))
    die(ERR_INT("CloseHandle() failed: %s", LastErr().get()));

  _impl.reset(new Pipe_impl(h_in_wr, h_out_rd, h_err_rd)); }

void OSI::Pipe::close_write() const noexcept {
  if (!_impl->out) return;
  if (!CloseHandle(_impl->out))
    die(ERR_INT("CloseHandle() failed: %s", LastErr().get()));
  _impl->out = nullptr; }

void OSI::Pipe::close() noexcept {
  if (_impl->out && !CloseHandle(_impl->out))
    die(ERR_INT("CloseHandle() failed: %s", LastErr().get()));
  while (_impl->in.getline_block());
  while (_impl->err.getline_block());  
  _impl.reset(nullptr); }

bool OSI::Pipe::ok() const noexcept {
  if (!_impl) return true;
  return _impl->out != nullptr && _impl->in.ok() && _impl->err.ok(); }

size_t OSI::Pipe::write(const char *msg, size_t n) const noexcept {
  if (UINT32_MAX < n) die(ERR_INT("bad len argument"));
  assert(msg);
  DWORD dw_written;
  if (!WriteFile(_impl->out, msg, static_cast<DWORD>(n), &dw_written, nullptr))
    die(ERR_INT("WriteFile() failed: %s", LastErr().get()));
  return static_cast<size_t>(dw_written); }

class OSI::Selector_impl {
  const Pipe *_pipes[MAXIMUM_WAIT_OBJECTS / 2U];
  uint _npipe;
public:
  void reset() noexcept { _npipe = 0; }
  void add(const Pipe &pipe) noexcept;
  
  void wait(uint sec, uint msec) noexcept {
    assert(msec < 1000U);
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD nh = 0;
    bool has_msg = false;
    
    for (uint u = 0; u < _npipe; ++u) {
      const Pipe &pipe = *( _pipes[u] );
      if (pipe.is_closed()) continue;
      
      if (!pipe._impl->done_in) {
	pipe._impl->ready_in = pipe._impl->in.getline();
	if (pipe._impl->in.is_eof() || pipe._impl->ready_in) {
	  has_msg = true; break; }
	else handles[nh++] = pipe._impl->in.get_event(); }
      
      if (!pipe._impl->done_err) {
	pipe._impl->ready_err = pipe._impl->err.getline();
	if (pipe._impl->err.is_eof() || pipe._impl->ready_err) {
	  has_msg = true; break; }
	else handles[nh++] = pipe._impl->err.get_event(); } }

    if (has_msg);
    else if (nh == 0) Sleep(sec * 1000U + msec);
    else {
      DWORD dw = WaitForMultipleObjects(nh, handles, FALSE,
					sec * 1000U + msec);
      if (dw == WAIT_FAILED)
	die(ERR_INT("WaitForMultipleObjects() failed: %s", LastErr().get()));
      if (dw == WAIT_TIMEOUT) return; }
    
    for (uint u = 0; u < _npipe; ++u) {
      const Pipe &pipe = *( _pipes[u] );
      if (pipe.is_closed()) continue;
      
      if (!pipe._impl->done_in
	  && !pipe._impl->in.is_eof()
	  && !pipe._impl->ready_in
	  && (WaitForSingleObject(pipe._impl->in.get_event(), 0)
	      == WAIT_OBJECT_0)) {
	pipe._impl->in.load();
	pipe._impl->ready_in = pipe._impl->in.getline(); }
      
      if (!pipe._impl->done_err
	  && !pipe._impl->err.is_eof()
	  && !pipe._impl->ready_err
	  && (WaitForSingleObject(pipe._impl->err.get_event(), 0)
	      == WAIT_OBJECT_0)) {
	pipe._impl->err.load();
	pipe._impl->ready_err = pipe._impl->err.getline(); } } }
  
  bool try_getline_in(const Pipe &pipe, char **pmsg) const noexcept;
  bool try_getline_err(const Pipe &pipe, char **pmsg) const noexcept; };

class OSI::Dir_impl {
  WIN32_FIND_DATA _ffd;
  HANDLE _hfind;
  bool _is_first, _is_end;

public:
  explicit Dir_impl(const char *dname) noexcept : _is_first(true),
						  _is_end(false) {
    FName fname(dname, "/*");
    _hfind = FindFirstFileA(fname.get_fname(), &_ffd);
    if (_hfind == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND) _is_end = true;
      else die(ERR_INT("cannot open directory %s", dname)); } }
  
  ~Dir_impl() noexcept {
    if (!FindClose(_hfind))
      die(ERR_INT("FindClose() failed: %s", LastErr().get())); }

  const char *next() noexcept {
    if (_is_end) return nullptr;
    if (_is_first) { _is_first = false; return _ffd.cFileName; }
    if (!FindNextFileA(_hfind, &_ffd)) {
      if (GetLastError() == ERROR_NO_MORE_FILES) {
	_is_end = true; return nullptr; }
      else die(ERR_INT("FindNextFileA() failed: %s", LastErr().get())); }
    return _ffd.cFileName; }
};

volatile atomic<handler_t> handler;
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) noexcept {
  switch (fdwCtrlType) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    (*handler)(static_cast<int>(fdwCtrlType + 1U));
    return TRUE;

  default:
    return FALSE; } }

void OSI::handle_signal(handler_t h) noexcept {
  handler = h;
  if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
    die(ERR_INT("SetConsoleCtrlHandler() failed: %s", LastErr().get())); }

void OSI::prevent_multirun(const FName &fname) noexcept {
  if (CreateMutexA(nullptr, TRUE, fname.get_fname())) return;
  if (GetLastError() != ERROR_ALREADY_EXISTS)
    die(ERR_INT("SetConsoleCtrlHandler() failed: %s", LastErr().get()));
  die(ERR_INT("another instance is running")); }

char *OSI::strtok(char *str, const char *delim, char **saveptr) noexcept {
  assert(delim && saveptr);
  return strtok_s(str, delim, saveptr); }

class ScktStartup {
  WSADATA _wsaData;
public:
  explicit ScktStartup() noexcept {
    int ret = WSAStartup(MAKEWORD(2, 2), &_wsaData);
    if (ret != NO_ERROR)
      die(ERR_INT("WSAStartup() failed with code %d.", ret)); }
  ~ScktStartup() noexcept { WSACleanup(); }
};
static ScktStartup ss;

class OSI::Conn_impl {
  sockaddr_in _s_addr;
  SOCKET _sckt;
  
  void connect() {
    _sckt = socket(AF_INET, SOCK_STREAM, 0);
    if (_sckt == INVALID_SOCKET)
      die(ERR_INT("socket() failed: %s", LastErr(LastErr::sckt).get()));
  
    if (::connect(_sckt, reinterpret_cast<const sockaddr *>(&_s_addr),
		  sizeof(_s_addr)) == SOCKET_ERROR) {
      ErrInt e = ERR_INT("connect() failed: %s", LastErr(LastErr::sckt).get());
      close();
      throw e; } }
  
  void close() const noexcept {
    if (closesocket(_sckt) == SOCKET_ERROR)
      die(ERR_INT("closesocket() failed: %s", LastErr(LastErr::sckt).get())); }

  bool wait_recv(uint sec) const noexcept {
    timeval tv;
    tv.tv_sec  = sec;
    tv.tv_usec = 0;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(_sckt, &fds);
    int ret = select(0, &fds, nullptr, nullptr, &tv);
    if (ret < 0) die(ERR_INT("select() failed: %s",
			     LastErr(LastErr::sckt).get()));
    return (ret == 0) ? false : true; }

  size_t recv(char *buf, size_t len) const {
    assert(buf && 0 < len && len <= INT_MAX);
    int ret = ::recv(_sckt, buf, static_cast<int>(len), 0);
    if (ret == SOCKET_ERROR)
      throw ERR_INT("recv() failed: %s", LastErr(LastErr::sckt).get());
    return static_cast<uint>(ret); }
  
  bool wait_send(uint sec) const noexcept {
    timeval tv;
    tv.tv_sec  = sec;
    tv.tv_usec = 0;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(_sckt, &fds);
    int ret = select(0, nullptr, &fds, nullptr, &tv);
    if (ret < 0) die(ERR_INT("select() failed: %s",
			     LastErr(LastErr::sckt).get()));
    return (ret == 0) ? false : true; }
  
  size_t send(const char *buf, size_t len) const {
    assert(buf && 0 < len && len <= INT_MAX);
    int ret = ::send(_sckt, buf, static_cast<int>(len), 0);
    if (ret == SOCKET_ERROR)
      throw ERR_INT("send() failed: %s", LastErr(LastErr::sckt).get());
    return static_cast<uint>(ret); }

public:
  explicit Conn_impl(const char *saddr, uint port);
  ~Conn_impl() noexcept { close(); }
  bool ok() const noexcept { return _sckt != INVALID_SOCKET; }
  void send(const char *buf, size_t len_send, uint tout, uint bufsiz) const;
  void recv(char *buf, size_t len_tot, uint tout, uint bufsiz) const; };

static_assert(BUFSIZ <= UINT32_MAX, "BUFSIZ too large");
#else
#  include <algorithm>
#  include <csignal>
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/file.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  define MAXIMUM_WAIT_OBJECTS 64
using std::max;

class PipeIn_impl {
  size_t _len_buf, _len_line;
  int _fd;
  bool _is_eof, _got_eof;
  char _buf[BUFSIZ], _line[65536];
  
public:
  explicit PipeIn_impl(int fd) noexcept
    : _len_buf(0), _len_line(0), _fd(fd), _is_eof(false), _got_eof(false) {}

  ~PipeIn_impl() noexcept {
    if (!_is_eof) die(ERR_INT("INTERNAL ERROR"));
    if (::close(_fd) < 0) die(ERR_CLL("close")); }
  
  void load() noexcept {
    if (_got_eof) _is_eof = true;
    if (_is_eof) return;

    ssize_t ret = ::read(_fd, _buf, sizeof(_buf));
    if (ret < 0) die(ERR_CLL("read"));
    if (ret == 0 && _len_line == 0) _is_eof = true;
    else if (ret == 0) {
      _got_eof = true;
      _buf[0]  = '\n';
      ret      = 1; }
    _len_buf = static_cast<size_t>(ret); }
  
  char *getline_block() noexcept;

  char *getline() noexcept {
    if (_is_eof) return nullptr;
    
    size_t pos = 0;
    for (; pos < _len_buf && _buf[pos] != '\n'; ++pos) {
      if (sizeof(_line) <= _len_line + 1U) die(ERR_INT("buffer overrun"));
      _line[_len_line++] = _buf[pos]; }

    if (pos == _len_buf) { _len_buf = 0; return nullptr; }
    
    assert(pos < _len_buf);
    pos      += 1U;
    _len_buf -= pos;
    memmove(_buf, _buf + pos, _len_buf);

    assert(_len_line < sizeof(_line));
    _line[_len_line] = '\0';
    _len_line = 0;
    return _line; }

  bool is_eof() const noexcept { return _is_eof; }
  bool ok() const noexcept { return 0 <= _fd; }
  int get_fd() const noexcept { return _fd; } };

class OSI::Pipe_impl {
public:
  int out;
  pid_t pid;
  PipeIn_impl in, err;
  char *ready_in, *ready_err;
  bool done_in, done_err;
  explicit Pipe_impl(int out_, pid_t pid_, int in_, int err_) noexcept
    : out(out_), pid(pid_), in(in_), err(err_),
      done_in(false), done_err(false) {} };

void OSI::Pipe::open(const char *path, char * const argv[]) noexcept {
  assert(path && argv && argv[0]);
  enum { index_read = 0, index_write = 1 };
  int perr_c2p[2], pipe_c2p[2], pipe_p2c[2];
  if (pipe(perr_c2p) < 0) die(ERR_CLL("pipe"));
  if (pipe(pipe_c2p) < 0) die(ERR_CLL("pipe"));
  if (pipe(pipe_p2c) < 0) die(ERR_CLL("pipe"));

  pid_t pid = fork();
  if (pid < 0) die(ERR_CLL("fork"));
  if (pid == 0) {
    ::close(pipe_p2c[index_write]);
    ::close(pipe_c2p[index_read]);
    ::close(perr_c2p[index_read]);
    dup2(pipe_p2c[index_read], 0);
    dup2(pipe_c2p[index_write], 1);
    dup2(perr_c2p[index_write], 2);
    ::close(pipe_p2c[index_read]);
    ::close(pipe_c2p[index_write]);
    ::close(perr_c2p[index_write]);
    if (execv(path, argv) < 0) die(ERR_CLL("execv")); }

  ::close(pipe_p2c[index_read]);
  ::close(pipe_c2p[index_write]);
  ::close(perr_c2p[index_write]);
  _impl.reset(new Pipe_impl(pipe_p2c[index_write], pid, pipe_c2p[index_read],
			    perr_c2p[index_read])); }

void OSI::Pipe::close_write() const noexcept {
  if (_impl->out < 0) return;
  if (::close(_impl->out) < 0) die(ERR_CLL("close"));
  _impl->out = -1; }

void OSI::Pipe::close() noexcept {
  if (0 <= _impl->out && ::close(_impl->out) < 0) die(ERR_CLL("close"));
  while (_impl->in.getline_block());
  while (_impl->err.getline_block());
  if (waitpid(_impl->pid, nullptr, 0) < 0) die(ERR_CLL("waitpid"));
  _impl.reset(nullptr); }

bool OSI::Pipe::ok() const noexcept {
  return !_impl || (0 <= _impl->out && _impl->in.ok()
		    && _impl->err.ok() && 0 < _impl->pid); }

size_t OSI::Pipe::write(const char *msg, size_t n) const noexcept {
  assert(msg);
  ssize_t ret = ::write(_impl->out, msg, n);
  if (ret < 0) die(ERR_CLL("write"));
  return static_cast<size_t>(ret); }

class OSI::Dir_impl {
  DIR *_pd;
  dirent *_pent;
public:
  explicit Dir_impl(const char *dname) noexcept {
    _pd = opendir(dname);
    if (!_pd) die(ERR_INT("cannot open directory %s", dname)); }
  
  ~Dir_impl() noexcept { if (closedir(_pd) < 0) die(ERR_CLL("closedir")); }

  const char *next() noexcept {
    errno = 0;
    _pent = readdir(_pd);
    if (_pent) return _pent->d_name;
    if (errno) die(ERR_CLL("readdir"));
    return nullptr; } };

void OSI::handle_signal(handler_t h) noexcept {
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) die(ERR_CLL("signal"));
  if (signal(SIGHUP,  h) == SIG_ERR) die(ERR_CLL("signal"));
  if (signal(SIGTERM, h) == SIG_ERR) die(ERR_CLL("signal"));
  if (signal(SIGINT,  h) == SIG_ERR) die(ERR_CLL("signal")); }

void OSI::prevent_multirun(const FName &fname) noexcept {
  int fd = open(fname.get_fname(), O_CREAT | O_RDWR, 0666);
  if (flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK)
    die(ERR_INT("another instance is running")); }

class OSI::Selector_impl {
  const Pipe *_pipes[MAXIMUM_WAIT_OBJECTS / 2U];
  uint _npipe;
public:
  void reset() noexcept { _npipe = 0; }
  void add(const Pipe &pipe) noexcept;

  void wait(uint sec, uint msec) noexcept {
    assert(msec < 1000U);
    fd_set _rfds;
    int _fd_max;
    bool has_msg = false;
    
    FD_ZERO(&_rfds);
    _fd_max = -1;
    for (uint u = 0; u < _npipe; ++u) {
      const Pipe &pipe = *( _pipes[u] );
      if (pipe.is_closed()) continue;
      
      if (!pipe._impl->done_in) {
	pipe._impl->ready_in = pipe._impl->in.getline();
	if (pipe._impl->in.is_eof() || pipe._impl->ready_in) has_msg = true;
	else {
	  FD_SET(pipe._impl->in.get_fd(), &_rfds);
	  _fd_max = max(_fd_max, pipe._impl->in.get_fd()); } }
      
      if (!pipe._impl->done_err) {
	pipe._impl->ready_err = pipe._impl->err.getline();
	if (pipe._impl->err.is_eof() || pipe._impl->ready_err) has_msg = true;
	else {
	  FD_SET(pipe._impl->err.get_fd(), &_rfds);
	  _fd_max = max(_fd_max, pipe._impl->err.get_fd()); } } }

    timeval tv;
    if (has_msg) {
      tv.tv_sec  = 0;
      tv.tv_usec = 0; }
    else {
      tv.tv_sec  = sec;
      tv.tv_usec = msec * 1000U; }
    if (select(_fd_max + 1, &_rfds, nullptr, nullptr, &tv) < 0)
      die(ERR_CLL("select"));
    
    for (uint u = 0; u < _npipe; ++u) {
      const Pipe &pipe = *( _pipes[u] );
      if (pipe.is_closed()) continue;
      
      if (!pipe._impl->done_in
	  && !pipe._impl->in.is_eof()
	  && !pipe._impl->ready_in
	  && FD_ISSET(pipe._impl->in.get_fd(), &_rfds)) {
	pipe._impl->in.load();
	pipe._impl->ready_in = pipe._impl->in.getline(); }
      
      if (!pipe._impl->done_err
	  && !pipe._impl->err.is_eof()
	  && !pipe._impl->ready_err
	  && FD_ISSET(pipe._impl->err.get_fd(), &_rfds)) {
	pipe._impl->err.load();
	pipe._impl->ready_err = pipe._impl->err.getline(); } }
  }
  
  bool try_getline_in(const Pipe &pipe, char **pmsg) const noexcept;
  bool try_getline_err(const Pipe &pipe, char **pmsg) const noexcept; };

char *OSI::strtok(char *str, const char *delim, char **saveptr) noexcept {
  assert(delim && saveptr);
  return strtok_r(str, delim, saveptr); }

class OSI::Conn_impl {
  sockaddr_in _s_addr;
  int _sckt;
  
  void connect() {
    _sckt = socket(AF_INET, SOCK_STREAM, 0);
    if (_sckt < 0) die(ERR_CLL("socket"));
  
    if (::connect(_sckt, reinterpret_cast<const sockaddr *>(&_s_addr),
		  sizeof(_s_addr)) < 0) {
      ErrCLL e = ERR_CLL("connect");
      close();
      throw e; } }
      
  void close() const noexcept {
    if (::close(_sckt) < 0) die(ERR_CLL("close")); }

  bool wait_recv(uint sec) const noexcept {
    timeval tv;
    tv.tv_sec  = sec;
    tv.tv_usec = 0;
  
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(_sckt, &fds);
    int ret = select(_sckt + 1, &fds, nullptr, nullptr, &tv);
    if (ret < 0) die(ERR_CLL("select"));
    return (ret == 0) ? false : true; }
  
  size_t recv(char *buf, size_t len) const {
    assert(buf && 0 < len);
    ssize_t sret = ::recv(_sckt, buf, len, 0);
    if (sret < 0) throw ERR_CLL("recv");
    return static_cast<size_t>(sret); }
  
  bool wait_send(uint sec) const noexcept {
    timeval tv;
    tv.tv_sec  = sec;
    tv.tv_usec = 0;
  
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(_sckt, &fds);
    int ret = select(_sckt + 1, nullptr, &fds, nullptr, &tv);
    if (ret < 0) die(ERR_CLL("select"));
    return (ret == 0) ? false : true; }
  
  size_t send(const char *buf, size_t len) const {
    assert(buf && 0 < len);
    ssize_t sret = ::send(_sckt, buf, len, 0);
    if (sret < 0) throw ERR_CLL("send");
    return static_cast<size_t>(sret); }

public:
  explicit Conn_impl(const char *saddr, uint port);
  ~Conn_impl() noexcept { close(); }
  bool ok() const noexcept { return 0 <= _sckt; }
  void send(const char *buf, size_t len_send, uint tout, uint bufsiz) const;
  void recv(char *buf, size_t len_tot, uint tout, uint bufsiz) const; };

#endif

static_assert(INET_ADDRSTRLEN <= 24, "INET_ADDRSTLEN is not 16.");

char *PipeIn_impl::getline_block() noexcept {
  while (!is_eof()) {
    char *ret = getline();
    if (ret) return ret;
    load(); }
  return nullptr; }
    
OSI::Pipe::Pipe() noexcept : _impl(nullptr) {}
OSI::Pipe::~Pipe() noexcept {}
bool OSI::Pipe::is_closed() const noexcept { return !_impl; }
char *OSI::Pipe::getline_in() const noexcept {
  return _impl->in.getline(); }
char *OSI::Pipe::getline_err() const noexcept {
  return _impl->err.getline(); }
char *OSI::Pipe::getline_in_block() const noexcept {
  return _impl->in.getline_block(); }
char *OSI::Pipe::getline_err_block() const noexcept {
  return _impl->err.getline_block(); }

void OSI::Selector_impl::add(const Pipe &pipe) noexcept {
  assert(pipe.ok());
  if (MAXIMUM_WAIT_OBJECTS < _npipe * 2U)
    die(ERR_INT("Npipe exceeds MAXIMUM_WAIT_OBJECTS."));
  _pipes[_npipe++] = &pipe; }

bool OSI::Selector_impl::try_getline_in(const Pipe &pipe,
					  char **pmsg) const noexcept {
  assert(pipe.ok() && pmsg);
  if (pipe.is_closed()) return false;
  if (pipe._impl->done_in) return false;
  if (pipe._impl->ready_in) {
    *pmsg = pipe._impl->ready_in;
    return true; }
  else if (pipe._impl->in.is_eof()) {
    pipe._impl->done_in = true;
    *pmsg = nullptr;
    return true; }
  return false; }

bool OSI::Selector_impl::try_getline_err(const Pipe &pipe,
					   char **pmsg) const noexcept {
  assert(pipe.ok() && pmsg);
  if (pipe.is_closed()) return false;
  if (pipe._impl->done_err) return false;
  if (pipe._impl->ready_err) {
    *pmsg = pipe._impl->ready_err;
    return true; }
  if (pipe._impl->err.is_eof()) {
    pipe._impl->done_err = true;
    *pmsg = nullptr;
    return true; }
  return false; }

OSI::Selector::Selector() noexcept : _impl(new Selector_impl) {}
OSI::Selector::~Selector() noexcept {}
void OSI::Selector::reset() const noexcept { _impl->reset(); }
void OSI::Selector::add(const Pipe &pipe) const noexcept {
  _impl->add(pipe); }
void OSI::Selector::wait(uint sec, uint msec) const noexcept {
  _impl->wait(sec, msec); }
bool OSI::Selector::try_getline_in(const Pipe &pipe, char **pmsg)
  const noexcept { return _impl->try_getline_in(pipe, pmsg); }
bool OSI::Selector::try_getline_err(const Pipe &pipe, char **pmsg)
  const noexcept { return _impl->try_getline_err(pipe, pmsg); }

OSI::Conn_impl::Conn_impl(const char *saddr, uint port) {
  assert(saddr && port <= UINT16_MAX);
  memset(&_s_addr, 0, sizeof(_s_addr));
  _s_addr.sin_family      = AF_INET;
  _s_addr.sin_port        = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, saddr, &( _s_addr.sin_addr )) < 1)
    die(ERR_INT("bad address %s", saddr));
  connect(); }

void OSI::Conn_impl::send(const char *buf, size_t len_send, uint tout,
			    uint bufsiz) const {
  assert(buf && 0 < len_send && 0 < tout && 0 < bufsiz);
  size_t len_sent = 0;
  while (len_sent < len_send) {
    if (!wait_send(tout)) throw ERR_INT("select for send timeout");
    
    size_t len_avail = len_send - len_sent;
    len_avail = min(len_avail, static_cast<size_t>(bufsiz));
    len_sent += send(buf + len_sent, len_avail); }
  assert(len_sent == len_send); }
    
void OSI::Conn_impl::recv(char *buf, size_t len_tot, uint tout,
			    uint bufsiz) const {
  assert(buf && 0 < len_tot && 0 < tout && 0 < bufsiz);
  size_t len_read = 0;
  while (len_read < len_tot) {
    if (!wait_recv(tout)) throw ERR_INT("select for recv timeout");
    
    size_t len_avail = len_tot - len_read;
    len_avail = min(len_avail, static_cast<size_t>(bufsiz));
    size_t ret = recv(buf + len_read, len_avail);
    if (ret == 0) throw ERR_INT("server closed connection");
    len_read += ret; }
  assert(len_read == len_tot); }

OSI::Conn::Conn(const char *saddr, uint port)
  : _impl(new Conn_impl(saddr, port)) {}
OSI::Conn::~Conn() noexcept {}
bool OSI::Conn::ok() const noexcept { return _impl && _impl->ok(); }
void OSI::Conn::send(const char *buf, size_t len, uint timeout, uint bufsiz)
  const { return _impl->send(buf, len, timeout, bufsiz); }
void OSI::Conn::recv(char *buf, size_t len, uint timeout, uint bufsiz)
  const { return _impl->recv(buf, len, timeout, bufsiz); }

OSI::IAddr::IAddr(const char *p, uint port) noexcept
  : _port(static_cast<uint16_t>(port)) {
  assert(p);
  if (sizeof(_cipv4) < strlen(p) + 1U) die(ERR_INT("bad address %s", p));
  strcpy(_cipv4, p);
  
  in_addr sin_addr;
  if (inet_pton(AF_INET, p, &sin_addr) < 1) die(ERR_INT("bad address %s", p));
  _crc64 = XZAux::crc64(_cipv4, 0);
  _addr  = sin_addr.s_addr; }

void OSI::IAddr::set_iaddr(const sockaddr_in &c_addr) noexcept {
  if (!inet_ntop(AF_INET, &c_addr.sin_addr, _cipv4, sizeof(_cipv4)))
    die(ERR_CLL("inet_ntop"));
  _crc64 = XZAux::crc64(_cipv4, 0);
  _addr  = c_addr.sin_addr.s_addr;
  _port  = ntohs(c_addr.sin_port); }

OSI::Dir::Dir(const char *dname) noexcept : _impl(new Dir_impl(dname)) {}
OSI::Dir::~Dir() noexcept {}
const char * OSI::Dir::next() const noexcept { return _impl->next(); }

static void binary2text(char *msg, size_t &len, char &ch_last) noexcept {
  assert(msg);
  size_t len1 = 0;
  for (size_t len0 = 0; len0 < len; ++len0) {
    char ch = msg[len0];
    if ((ch_last == '\r' && ch == '\n') || (ch_last == '\n' && ch == '\r'));
    else if (ch == '\r') msg[len1++] = '\n';
    else msg[len1++] = ch;
    ch_last = ch; }
  len = len1; }
