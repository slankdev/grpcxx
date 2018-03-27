
#include <iostream>
#include <vector>
#include <string>
#include <stdarg.h>
#include <unistd.h>
#include <grpcpp/grpcpp.h>
#include "openconfig.grpc.pb.h"
#include "openconfig.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::Status;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;

using openconfig::Register;
using openconfig::RegisterRequest;
using openconfig::RegisterReply;
using openconfig::RegisterModuleRequest;
using openconfig::RegisterModuleReply;
using openconfig::Config;
using openconfig::ConfigRequest;
using openconfig::ConfigReply;
using openconfig::Show;
using openconfig::ShowRequest;
using openconfig::ShowReply;
using openconfig::Exec;
using openconfig::ExecRequest;
using openconfig::ExecReply;

volatile bool force_quit = false;

namespace fmt {
  inline std::string sprintf(const char* fmt_, ...)
  {
    char str[1000];
    va_list args;
    va_start(args, fmt_);
    vsprintf(str, fmt_, args);
    va_end(args);
    return str;
  }

  std::vector<std::string> split(const std::string &str, char sep)
  {
    std::vector <std::string> v;
    auto first = str.begin ();
    while ( first != str.end () )
      {
        auto last = first;
        while ( last != str.end () && *last != sep )
          ++last;
        v.push_back (std::string (first, last));
        if( last != str.end () )
          ++last;
        first = last;
      }
    return v;
  }
} /* namespace fmt */

typedef struct openconfigd_client
{
  public:

    std::unique_ptr < ClientReaderWriter
      <ConfigRequest, ConfigReply> > config_stream;

    openconfigd_client (std::shared_ptr<Channel> channel)
      : register_stub_ (Register::NewStub (channel))
      , config_stub_ (Config::NewStub (channel)) {}

    void
    InstallCommand (std::string name,
                    std::string module,
                    std::string line,
                    std::string mode,
                    std::string helps,
                    int32_t privilege)
    {
      RegisterRequest req;
      req.set_name(name);
      req.set_module(module);
      req.set_line(line);
      req.set_mode(mode);
      std::vector <std::string> helps_vec =
        fmt::split (std::string (helps), '\n');
      for (std::string& help : helps_vec) {
        req.add_helps(help);
      }
      req.set_privilege(1);
      req.set_code(openconfig::REDIRECT_SHOW);

      RegisterReply rep;
      ClientContext ctx;
      Status status = register_stub_->DoRegister(&ctx, req, &rep);
      if (!status.ok())
        {
          fprintf(stderr, "Failed command register [%s]\n",
              req.line().c_str());
          return;
        }
    }

#if 0
    // TODO: not tested
    void
    DoConfig_Read (ConfigReply* rep)
    {
      bool ret = config_stream->Read (rep);
      if (! ret)
        {
          fprintf (stderr, "Failed read config\n");
          exit (-1);
        }
    }

    // TODO: not tested
    void
    DoConfig_Write (const ConfigRequest& req)
    {
      bool ret = config_stream->Write (req);
      if (! ret)
        {
          fprintf (stderr, "Failed write config\n");
          exit (-1);
        }
    }

    // TODO: not tested
    void
    DoConfig_WritesDone ()
    {
      bool ret = this->config_stream->WritesDone ();
      if (! ret)
        {
          fprintf (stderr, "Failed write done\n");
          exit (-1);
        }
    }
#endif

    void
    DoConfig (const char* modname, int modport)
    {
      ClientContext ctx;
      this->config_stream = config_stub_->DoConfig (&ctx);

      ConfigRequest req;
      req.set_type (openconfig::SUBSCRIBE_MULTI);
      req.set_module (modname);
      req.set_port (modport);
      req.add_path ("interfaces");
      // req.add_path ("protocols");
      // req.add_path ("policy");
      bool ret = config_stream->Write (req);
      if (!ret) exit(1);

      while (!force_quit)
        {
          ConfigReply rep;
          bool ret = config_stream->Read (&rep);
          if (!ret) break;

          printf ("recv\n");
          printf (" rep.result: %u\n", rep.result());
          printf (" rep.type  : %u\n", rep.type());
          printf (" rep.path.size: %d \n", rep.path().size());
          for (size_t i=0; i<rep.path().size(); i++)
            printf ("    path[%zd]: %s \n", i, rep.path()[i].c_str());
        }

      config_stream->WritesDone ();
    }

    void
    DoRegisterModule (const char* modname, int modport)
    {
      RegisterModuleRequest req;
      req.set_module (modname);
      req.set_port (fmt::sprintf ("%d", modport));
      printf ("register module module=%s, port=%d\n",
          modname, modport);

      RegisterModuleReply rep;
      ClientContext ctx;

      Status status = register_stub_->DoRegisterModule (&ctx, req, &rep);
      if (! status.ok ())
        {
          fprintf (stderr, "Failed rpc DoRegisterModule\n");
          exit(1);
        }
    }
  private:
    std::unique_ptr <Register::Stub> register_stub_;
    std::unique_ptr <Config::Stub> config_stub_;
} openconfigd_client_t;

openconfigd_client_t*
openconfigd_client_create (const char* remote, const char* modname, int modport)
{
  auto channel = grpc::CreateChannel (remote,
      grpc::InsecureChannelCredentials ());
  openconfigd_client_t* client = new openconfigd_client (channel);
  client->DoRegisterModule (modname, modport);
  return client;
}

void
openconfigd_client_free (openconfigd_client_t* client)
{
  delete client;
}

void
openconfigd_DoConfig (openconfigd_client_t* client, const char* modname, int modport)
{
  client->DoConfig (modname, modport);
}

void
openconfigd_InstallCommand (
        openconfigd_client_t* client,
        const char* name,
        const char* module,
        const char* line,
        const char* helps,
        int32_t privilege)
{
  client->InstallCommand (name, module,
      line, "exec", helps, privilege);
}

typedef struct openconfigd_vty
{
  std::string str;
} openconfigd_vty_t;

void openconfigd_printf (
    openconfigd_vty_t* vty,
    const char* fmt_, ...)
{
  char str[1000];
  va_list args;
  va_start(args, fmt_);
  vsprintf(str, fmt_, args);
  va_end(args);
  vty->str += str;
}

typedef struct openconfigd_show_service final : public Show::Service
{
  public:

    Status
    Show (ServerContext* ctx, const ShowRequest* req,
        ServerWriter <ShowReply>* writer) override
    {
      if (callback)
        {
          char* argv [100];
          std::vector <std::string> args =
                fmt::split (req->line (), ' ');
          int argc = args.size ();
          for (size_t i=0; i<argc; i++)
            argv[i] = &args[i][0];

          openconfigd_vty_t vty;
          callback (argc, argv, &vty);

          ShowReply rep;
          rep.set_str (vty.str);
          writer->Write (rep);
        }
      else
        {
          ShowReply rep;
          rep.set_str ("command is not installed.");
          writer->Write (rep);
          assert (false);
        }

      return Status::OK;
    }

  public:
    openconfigd_show_service_cbfunc_t callback;
} openconfigd_show_service_t;

void
openconfigd_show_service_set_callback (openconfigd_show_service_t* service,
        openconfigd_show_service_cbfunc_t fun)
{
  service->callback = fun;
}

openconfigd_show_service_t*
openconfigd_show_service_create ()
{
  openconfigd_show_service_t* ret = new openconfigd_show_service;
  return ret;
}

void
openconfigd_show_service_free (openconfigd_show_service_t* service)
{
  delete service;
}

typedef struct openconfigd_exec_service final : public Exec::Service
{

    Status
    DoExec (ServerContext* ctx, const ExecRequest* req, ExecReply* rep) override
    {
      return Status::OK;
    }

} openconfigd_exec_service_t;

openconfigd_exec_service_t*
openconfigd_exec_service_create ()
{
  return new openconfigd_exec_service;
}

void
openconfigd_exec_service_free (openconfigd_exec_service_t* service)
{
  delete service;
}


