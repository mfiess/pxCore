
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <stdint.h>
#include <netinet/in.h>
#include <rtObject.h>
#include <rapidjson/document.h>

#include "rtRpcTypes.h"
#include "rtRemoteObjectResolver.h"
#include "rtSocketUtils.h"

class rtRpcClient;

class rtRemoteObjectLocator
{
  friend class rtCommandDispatcher;

public:
  rtRemoteObjectLocator();
  ~rtRemoteObjectLocator();

public:
  rtError open(char const* dstaddr, uint16_t dstport, char const* srcaddr);
  rtError start();

  rtError registerObject(std::string const& name, rtObjectRef const& obj);
  rtError findObject(std::string const& name, rtObjectRef& obj, uint32_t timeout = 1000);

private:
  struct connected_client
  {
    sockaddr_storage    peer;
    int                 fd;
  };

  typedef rtError (rtRemoteObjectLocator::*command_handler_t)(rtJsonDocPtr_t const&, int fd, sockaddr_storage const& soc);

  void runListener();
  rtError doReadn(int fd, rt_sockbuf_t& buff, sockaddr_storage const& peer);
  void doAccept(int fd);
  void doDispatch(rtJsonDocPtr_t const& doc, int fd, sockaddr_storage const& peer);

  rtError openRpcListener();

  // command handlers
  // rtError on_search(rapidjson::Document const& doc, sockaddr* soc, socklen_t len);
  // rtError on_locate(rapidjson::Document const& doc, sockaddr* soc, socklen_t len);
  rtError onOpenSession(rtJsonDocPtr_t const& doc, int fd, sockaddr_storage const& soc);
  rtError onGet(rtJsonDocPtr_t const& doc, int fd, sockaddr_storage const& soc);
  rtError onSet(rtJsonDocPtr_t const& doc, int fd, sockaddr_storage const& soc);
  rtError onMethodCall(rtJsonDocPtr_t const& doc, int fd, sockaddr_storage const& soc);
  rtError onKeepAlive(rtJsonDocPtr_t const& doc, int fd, sockaddr_storage const& soc);

  rtError onClientDisconnect(connected_client& client);

  rtObjectRef getObject(std::string const& id) const;

private:
  struct object_reference
  {
    rtObjectRef object;
    std::vector<int> client_fds;
  };

  typedef std::map< std::string, object_reference > refmap_t;
  typedef std::map< std::string, command_handler_t > cmd_handler_map_t;
  typedef std::vector< connected_client > client_list_t;
  typedef std::map< std::string, std::shared_ptr<rtRpcClient> > tport_map_t;

  sockaddr_storage                m_rpc_endpoint;
  int                             m_rpc_fd;
  std::unique_ptr<std::thread>    m_thread;
  mutable std::mutex              m_mutex;
  cmd_handler_map_t               m_command_handlers;

  refmap_t                        m_objects;
  client_list_t                   m_client_list;

  rtRemoteObjectResolver*         m_resolver;
  tport_map_t                     m_transports;
  int                             m_pipe_write;
  int                             m_pipe_read;
};
