#include "proxy.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <ctime>
#include <fstream>
#include <map>
#include <unordered_map>

#include "client_info.h"
#include "function.h"
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::ofstream logFile("/var/log/erss/proxy.log");
std::unordered_map<std::string, Response> Cache;
void proxy::run() {
  // Client_Info * client_info = new Client_Info();

  int temp_fd = build_server(this->port_num);
  if (temp_fd == -1) {
    return;
  }
  int client_fd;
  int id = 0;
  while (1) {
    std::string ip;
    client_fd = server_accept(temp_fd, &ip);
    if (client_fd == -1) {
      std::cout << "connect client error" << std::endl;
      continue;
    }
    pthread_t thread;
    pthread_mutex_lock(&mutex);
    Client_Info * client_info = new Client_Info();
    client_info->setFd(client_fd);
    client_info->setIP(ip);
    client_info->setID(id);
    id++;
    pthread_mutex_unlock(&mutex);
    pthread_create(&thread, NULL, handle, client_info);
  }
}

void * proxy::handle(void * info) {
  Client_Info * client_info = (Client_Info *)info;
  int client_fd = client_info->getFd();

  char req_msg[65536] = {0};
  int len = recv(client_fd, req_msg, sizeof(req_msg), 0);  // fisrt request from client
  if (len <= 0) {
    return NULL;
  }
  std::string input = std::string(req_msg, len);
  if (input == "" || input == "\r" || input == "\n" || input == "\r\n") {
    return NULL;
  }
  Request * parser = new Request(input);
  if (parser->method != "POST" && parser->method != "GET" &&
      parser->method != "CONNECT") {
    return NULL;
  }
  pthread_mutex_lock(&mutex);
  logFile << client_info->getID() << ": \"" << parser->line << "\" from "
          << client_info->getIP() << " @ " << getTime().append("\0");
  pthread_mutex_unlock(&mutex);

  std::cout << "received client request is:" << req_msg << "end" << std ::endl;
  const char * host = parser->host.c_str();
  const char * port = parser->port.c_str();

  int server_fd = build_client(host, port);  //connect to server
  if (server_fd == -1) {
    std::cout << "Error in build client!\n";
    return NULL;
  }

  if (parser->method == "CONNECT") {
    pthread_mutex_lock(&mutex);
    logFile << client_info->getID() << ": "
            << "Requesting \"" << parser->line << "\" from " << host << std::endl;
    pthread_mutex_unlock(&mutex);

    handleConnect(client_fd, server_fd, client_info->getID());
    pthread_mutex_lock(&mutex);
    logFile << client_info->getID() << ": Tunnel closed" << std::endl;
    pthread_mutex_unlock(&mutex);
  }
  else if (parser->method == "GET") {
    int id = client_info->getID();
    bool valid = false;
    std::unordered_map<std::string, Response>::iterator it = Cache.begin();
    it = Cache.find(parser->line);
    if (it == Cache.end()) {
      pthread_mutex_lock(&mutex);
      logFile << client_info->getID() << ": not in cache" << std::endl;
      pthread_mutex_unlock(&mutex);

      pthread_mutex_lock(&mutex);
      logFile << client_info->getID() << ": "
              << "Requesting \"" << parser->line << "\" from " << host << std::endl;
      pthread_mutex_unlock(&mutex);

      send(server_fd, req_msg, len, 0);
      handleGet(client_fd, server_fd, client_info->getID(), host, parser->line);
    }
    else {                            //find in cache
      if (it->second.nocache_flag) {  //has no-cache symbol
        if (revalidation(it->second, parser->input, server_fd, id) ==
            false) {  //check Etag and Last Modified
          ask_server(id, parser->line, req_msg, len, client_fd, server_fd, host);
        }
        else {
          use_cache(it->second, id, client_fd);
        }
      }
      else {
        valid =
            CheckTime(server_fd, *parser, parser->line, it->second, client_info->getID());
        if (!valid) {  //ask for server,check res and put in cache if needed
          ask_server(id, parser->line, req_msg, len, client_fd, server_fd, host);
        }
        else {  //send from cache
          use_cache(it->second, id, client_fd);
        }
      }
      printcache();
    }
  }
  else if (parser->method == "POST") {
    pthread_mutex_lock(&mutex);
    logFile << client_info->getID() << ": "
            << "Requesting \"" << parser->line << "\" from " << host << std::endl;
    pthread_mutex_unlock(&mutex);
    handlePOST(client_fd, server_fd, req_msg, len, client_info->getID(), host);
  }
  close(server_fd);
  close(client_fd);
  return NULL;
}

void proxy::ask_server(int id,
                       std::string line,
                       char * req_msg,
                       int len,
                       int client_fd,
                       int server_fd,
                       const char * host) {
  pthread_mutex_lock(&mutex);
  logFile << id << ": "
          << "Requesting \"" << line << "\" from " << host << std::endl;
  pthread_mutex_unlock(&mutex);

  send(server_fd, req_msg, len, 0);
  handleGet(client_fd, server_fd, id, host, line);
}
void proxy::use_cache(Response & res, int id, int client_fd) {
  char cache_res[res.getSize()];
  strcpy(cache_res, res.getResponse());
  send(client_fd, cache_res, res.getSize(), 0);
  pthread_mutex_lock(&mutex);
  logFile << id << ": Responding \"" << res.line << "\"" << std::endl;
  pthread_mutex_unlock(&mutex);
}
void proxy::printcache() {
  std::unordered_map<std::string, Response>::iterator it = Cache.begin();
  std::cout << "****************Cache****************-" << std::endl;
  while (it != Cache.end()) {
    std::cout << "---------------Check Begin---------------" << std::endl;
    std::cout << "req_line ====== " << it->first << std::endl;
    std::string header = it->second.response.substr(0, 300);
    std::cout << "response header in cache =======" << header << std::endl;
    std::cout << "---------------end of header---------------" << std::endl;
    ++it;
  }
  std::cout << "****************Size*************-" << std::endl;
  std::cout << "cache.size=" << Cache.size() << std::endl;
  std::cout << "****************Cache end*************-" << std::endl;
}
bool proxy::CheckTime(int server_fd,
                      Request & parser,
                      std::string req_line,
                      Response & rep,
                      int id) {
  //  std::cout << "[DEBUG] Response max-age: " << rep.max_age << std::endl;
  // std::cout << "[DEBUG] Response Expires: " << rep.exp_str << std::endl;
  // std::cout << "[DEBUG] Response Etag: " << rep.ETag << std::endl;

  if (rep.max_age != -1) {
    time_t curr_time = time(0);
    time_t rep_time = mktime(rep.response_time.getTimeStruct()) - 18000;
    int max_age = rep.max_age;
    if (rep_time + max_age <= curr_time) {
      Cache.erase(req_line);
      std::cout << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" << std::endl;
      std::cout << "max_age erase cache, cache_size now is===========" << Cache.size()
                << std::endl;
      time_t dead_time = mktime(rep.response_time.getTimeStruct()) + rep.max_age - 18000;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      pthread_mutex_lock(&mutex);
      logFile << id << ": in cache, but expired at " << t;
      pthread_mutex_unlock(&mutex);
      return false;
    }
  }

  if (rep.exp_str != "") {
    time_t curr_time = time(0);
    time_t expire_time = mktime(rep.expire_time.getTimeStruct()) - 18000;
    std::cout << "curr_time==" << curr_time << std::endl;
    std::cout << "expire_time==" << expire_time << std::endl;
    if (curr_time > expire_time) {
      Cache.erase(req_line);
      std::cout << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" << std::endl;
      std::cout << "expiretime erase cache, cache_size now is===========" << Cache.size()
                << std::endl;

      time_t dead_time = mktime(rep.expire_time.getTimeStruct()) - 18000;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      pthread_mutex_lock(&mutex);
      std::cout << "expired time in logfile = " << dead_time << std::endl;
      logFile << id << ": in cache, but expired at " << t;
      pthread_mutex_unlock(&mutex);

      return false;
    }
  }
  bool revalid = revalidation(rep, parser.input, server_fd, id);
  if (revalid == false) {
    return false;
  }
  pthread_mutex_lock(&mutex);
  logFile << id << ": in cache, valid" << std::endl;
  pthread_mutex_unlock(&mutex);
  return true;
  //Wed Feb 26 04:09:10 2020
}
bool proxy::revalidation(Response & rep, std::string input, int server_fd, int id) {
  if (rep.ETag == "" && rep.LastModified == "") {
    return true;
  }
  std::string changed_input = input;
  if (rep.ETag != "") {
    std::string add_etag = "If-None-Match: " + rep.ETag.append("\r\n");
    std::cout << "ADD_ETAG: " << add_etag << "end" << std::endl;
    changed_input = changed_input.insert(changed_input.length() - 2, add_etag);
  }
  if (rep.LastModified != "") {
    std::string add_modified = "If-Modified-Since: " + rep.LastModified.append("\r\n");
    std::cout << "ADD_MODIFIED: " << add_modified << "end" << std::endl;
    changed_input = changed_input.insert(changed_input.length() - 2, add_modified);
  }
  std::string req_msg_str = changed_input;
  std::cout << "DEBUG Insert after is: " << req_msg_str << "end" << std::endl;
  char req_new_msg[req_msg_str.size() + 1];
  int send_len;
  if ((send_len = send(server_fd, req_new_msg, req_msg_str.size() + 1, 0)) > 0) {
    std::cout << "Verify: Send success!\n";
  }
  char new_resp[65536] = {0};
  int new_len = recv(server_fd, &new_resp, sizeof(new_resp), 0);
  if (new_len <= 0) {
    std::cout << "[Verify] received from server failed in checktime" << std::endl;
  }
  std::string checknew(new_resp, new_len);
  std::cout << "[Verify] new return response: " << checknew << std::endl;
  if (checknew.find("HTTP/1.1 200 OK") != std::string::npos) {
    pthread_mutex_lock(&mutex);
    logFile << id << ": in cache, requires validation" << std::endl;
    pthread_mutex_unlock(&mutex);
    return false;
  }
  return true;  //use from cache
}
void proxy::handlePOST(int client_fd,
                       int server_fd,
                       char * req_msg,
                       int len,
                       int id,
                       const char * host) {
  int post_len = getLength(req_msg, len);  //get length of client request
  if (post_len != -1) {
    std::string request = sendContentLen(client_fd, req_msg, len, post_len);
    char send_request[request.length() + 1];
    strcpy(send_request, request.c_str());
    //std::cout << "begin sending to server" << std::endl;
    send(server_fd,
         send_request,
         sizeof(send_request),
         MSG_NOSIGNAL);  // send all the request info from client to server
    char response[65536] = {0};
    int response_len = recv(server_fd,
                            response,
                            sizeof(response),
                            MSG_WAITALL);  //first time received response from server
    if (response_len != 0) {
      Response res;
      res.ParseLine(req_msg, len);
      pthread_mutex_lock(&mutex);
      logFile << id << ": Received \"" << res.getLine() << "\" from " << host
              << std::endl;
      pthread_mutex_unlock(&mutex);

      std::cout << "receive response from server which is:" << response << std::endl;

      send(client_fd, response, response_len, MSG_NOSIGNAL);

      pthread_mutex_lock(&mutex);
      logFile << id << ": Responding \"" << res.getLine() << std::endl;
      pthread_mutex_unlock(&mutex);
    }
    else {
      std::cout << "server socket closed!\n";
    }
  }
}
void proxy::handleGet(int client_fd,
                      int server_fd,
                      int id,
                      const char * host,
                      std::string req_line) {
  char server_msg[65536] = {0};
  int mes_len = recv(server_fd,
                     server_msg,
                     sizeof(server_msg),
                     0);  //received first response from server(all header, part body)
  //TEST
  std::string temp(server_msg, 300);
  std::cout << "Receive server response is: " << temp << std::endl;
  //TEST END
  if (mes_len == 0) {
    return;
  }
  Response parse_res;
  parse_res.ParseLine(server_msg, mes_len);
  pthread_mutex_lock(&mutex);
  logFile << id << ": Received \"" << parse_res.getLine() << "\" from " << host
          << std::endl;
  pthread_mutex_unlock(&mutex);

  bool is_chunk = findChunk(server_msg, mes_len);
  if (is_chunk) {
    pthread_mutex_lock(&mutex);
    logFile << id << ": not cacheable because it is chunked" << std::endl;
    pthread_mutex_unlock(&mutex);

    send(client_fd, server_msg, mes_len, 0);  //send first response to server
    char chunked_msg[28000] = {0};
    while (1) {  //receive and send remaining message
      int len = recv(server_fd, chunked_msg, sizeof(chunked_msg), 0);
      if (len <= 0) {
        return;
      }
      send(client_fd, chunked_msg, len, 0);
    }
    ///////////////////
  }
  else {
    //int content_len = getLength(server_msg, mes_len);  //get content length
    //no-store--store in cache
    bool no_store = false;
    std::string server_msg_str(server_msg, mes_len);
    size_t nostore_pos;
    if ((nostore_pos = server_msg_str.find("no-store")) != std::string::npos) {
      no_store = true;
    }
    parse_res.ParseField(server_msg, mes_len);
    std::cout << "[DEBUG] Check parse_res.Etag is: " << parse_res.ETag << std::endl;
    int content_len = getLength(server_msg, mes_len);  //get content length
    if (content_len != -1) {
      //std::cout << "\n content_len = " << content_len << std::endl;
      std::string msg = sendContentLen(
          server_fd, server_msg, mes_len, content_len);  //get the entire message
      char send_response[msg.length()];
      strcpy(send_response, msg.c_str());
      parse_res.setEntireRes(msg);
      //std::cout << "Send client response is: " << send_response << std::endl;
      send(client_fd, send_response, sizeof(send_response), 0);
    }
    else {
      std::cout << "no content-length "
                   "field-----------------Return to client at once"
                << std::endl;
      std::string server_msg_str(server_msg, mes_len);
      parse_res.setEntireRes(server_msg_str);
      send(client_fd, server_msg, mes_len, 0);
    }
    std::string logrespond(server_msg, mes_len);
    size_t log_pos = logrespond.find_first_of("\r\n");
    std::string log_line = logrespond.substr(0, log_pos);
    pthread_mutex_lock(&mutex);
    logFile << id << ": Responding \"" << log_line << "\"" << std::endl;
    pthread_mutex_unlock(&mutex);
    printcachelog(parse_res, no_store, req_line, id);
  }
}

void proxy::printcachelog(Response & parse_res,
                          bool no_store,
                          std::string req_line,
                          int id) {
  if (parse_res.response.find("HTTP/1.1 200 OK") != std::string::npos) {
    if (no_store) {
      pthread_mutex_lock(&mutex);
      logFile << id << ": not cacheable becaues NO STORE" << std::endl;
      pthread_mutex_unlock(&mutex);
      return;
    }
    if (parse_res.max_age != -1) {
      time_t dead_time =
          mktime(parse_res.response_time.getTimeStruct()) + parse_res.max_age - 18000;
      struct tm * asc_time = gmtime(&dead_time);
      const char * t = asctime(asc_time);
      pthread_mutex_lock(&mutex);
      logFile << id << ": cached, expires at " << t << std::endl;
      pthread_mutex_unlock(&mutex);
    }
    else if (parse_res.exp_str != "") {
      pthread_mutex_lock(&mutex);
      logFile << id << ": cached, expires at " << parse_res.exp_str << std::endl;
      pthread_mutex_unlock(&mutex);
    }
    Response storedres(parse_res);
    if (Cache.size() > 10) {
      std::unordered_map<std::string, Response>::iterator it = Cache.begin();
      Cache.erase(it);
    }
    Cache.insert(std::pair<std::string, Response>(req_line, storedres));
  }
}

std::string proxy::sendContentLen(int send_fd,
                                  char * server_msg,
                                  int mes_len,
                                  int content_len) {
  int total_len = 0;
  int len = 0;
  std::string msg(server_msg, mes_len);
  char new_server_msg[65536] = {0};
  while (total_len < content_len) {
    len = recv(send_fd, new_server_msg, sizeof(new_server_msg), 0);
    //std::cout << "\n in while loop, received length = " << len << std::endl;
    std::string temp(new_server_msg, len);
    msg += temp;
    total_len += len;
  }
  //std::cout << "\n after while loop, total length = " << total_len << std::endl;
  return msg;
}

bool proxy::findChunk(char * server_msg, int mes_len) {
  std::string msg(server_msg, mes_len);
  size_t pos;
  if ((pos = msg.find("chunked")) != std::string::npos) {
    return true;
  }
  return false;
}

int proxy::getLength(char * server_msg, int mes_len) {
  //std::cout << "mes_len = " << mes_len << std::endl;
  std::string msg(server_msg, mes_len);
  size_t pos;
  if ((pos = msg.find("Content-Length: ")) != std::string::npos) {
    size_t head_end = msg.find("\r\n\r\n");
    //std::cout << "head_end=" << head_end << std::endl;
    int part_body_len = mes_len - static_cast<int>(head_end) - 8;
    //std::cout << "cast<head_end>=" << static_cast<int>(head_end) << std::endl;
    //std::cout << "part_body_len = " << part_body_len << std::endl;
    size_t end = msg.find("\r\n", pos);
    std::string content_len = msg.substr(pos + 16, end - pos - 16);
    //std::cout << "conten_length string is: " << content_len << std::endl;
    //std::cout << "string length = " << content_len.length() << std::endl;
    int num = 0;
    for (size_t i = 0; i < content_len.length(); i++) {
      num = num * 10 + (content_len[i] - '0');
    }
    //std::cout << "calculate num =" << num << std::endl;
    return num - part_body_len - 4;
  }
  return -1;
}

void proxy::handleConnect(int client_fd, int server_fd, int id) {
  send(client_fd, "HTTP/1.1 200 OK\r\n\r\n", 19, 0);
  pthread_mutex_lock(&mutex);
  logFile << id << ": Responding \"HTTP/1.1 200 OK\"" << std::endl;
  pthread_mutex_unlock(&mutex);
  fd_set readfds;
  int nfds = server_fd > client_fd ? server_fd + 1 : client_fd + 1;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    FD_SET(client_fd, &readfds);

    select(nfds, &readfds, NULL, NULL, NULL);
    int fd[2] = {server_fd, client_fd};
    int len;
    for (int i = 0; i < 2; i++) {
      char message[65536] = {0};
      if (FD_ISSET(fd[i], &readfds)) {
        len = recv(fd[i], message, sizeof(message), 0);
        if (len <= 0) {
          return;
        }
        else {
          if (send(fd[1 - i], message, len, 0) <= 0) {
            return;
          }
          // std::cout << "Error in Sending!\n" << fd[i] << std::endl;
        }
      }
      // send(fd[1 - actual_i], actual_message, len, 0);
    }
  }
}

std::string proxy::getTime() {
  time_t currTime = time(0);
  struct tm * nowTime = gmtime(&currTime);
  const char * t = asctime(nowTime);
  std::cout << "The current time UTC/GMT is: " << std::string(t) << std::endl;
  return std::string(t);
}