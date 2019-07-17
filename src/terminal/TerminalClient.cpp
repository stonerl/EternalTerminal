#include "TerminalClient.hpp"

#include "PsuedoTerminalConsole.hpp"

using namespace et;
namespace google {}
namespace gflags {}
using namespace google;
using namespace gflags;

DEFINE_string(u, "", "username to login");
DEFINE_string(host, "localhost", "host to join");
DEFINE_int32(port, 2022, "port to connect on");
DEFINE_string(c, "", "Command to run immediately after connecting");
DEFINE_string(
    prefix, "",
    "Command prefix to launch etserver/etterminal on the server side");
DEFINE_string(t, "",
              "Array of source:destination ports or "
              "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
              "10080:80,10443:443, 10090-10092:8000-8002)");
DEFINE_string(rt, "",
              "Array of source:destination ports or "
              "srcStart-srcEnd:dstStart-dstEnd (inclusive) port ranges (e.g. "
              "10080:80,10443:443, 10090-10092:8000-8002)");
DEFINE_string(jumphost, "", "jumphost between localhost and destination");
DEFINE_int32(jport, 2022, "port to connect on jumphost");
DEFINE_bool(x, false, "flag to kill all old sessions belonging to the user");
DEFINE_int32(v, 0, "verbose level");
DEFINE_bool(logtostdout, false, "log to stdout");
DEFINE_bool(silent, false, "If enabled, disable logging");
DEFINE_bool(noratelimit, false,
            "There's 1024 lines/second limit, which can be "
            "disabled based on different use case.");

TerminalClient::TerminalClient(std::shared_ptr<SocketHandler> _socketHandler,
                               const SocketEndpoint& _socketEndpoint,
                               const string& id, const string& passkey,
                               shared_ptr<Console> _console)
    : console(_console) {
  portForwardHandler =
      shared_ptr<PortForwardHandler>(new PortForwardHandler(_socketHandler));
  InitialPayload payload;
  if (_socketEndpoint.isJumphost()) {
    payload.set_jumphost(true);
  }

  globalClient = shared_ptr<ClientConnection>(
      new ClientConnection(_socketHandler, _socketEndpoint, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      if (globalClient->connect()) {
        globalClient->writePacket(
            Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
        break;
      } else {
        LOG(ERROR) << "Connecting to server failed: Connect timeout";
        connectFailCount++;
        if (connectFailCount == 3) {
          throw std::runtime_error("Connect Timeout");
        }
      }
    } catch (const runtime_error& err) {
      LOG(INFO) << "Could not make initial connection to server";
      cout << "Could not make initial connection to " << _socketEndpoint << ": "
           << err.what() << endl;
      exit(1);
    }
    break;
  }
  VLOG(1) << "Client created with id: " << globalClient->getId();
};

vector<pair<int, int>> parseRangesToPairs(const string& input) {
  vector<pair<int, int>> pairs;
  auto j = split(input, ',');
  for (auto& pair : j) {
    vector<string> sourceDestination = split(pair, ':');
    try {
      if (sourceDestination[0].find('-') != string::npos &&
          sourceDestination[1].find('-') != string::npos) {
        vector<string> sourcePortRange = split(sourceDestination[0], '-');
        int sourcePortStart = stoi(sourcePortRange[0]);
        int sourcePortEnd = stoi(sourcePortRange[1]);

        vector<string> destinationPortRange = split(sourceDestination[1], '-');
        int destinationPortStart = stoi(destinationPortRange[0]);
        int destinationPortEnd = stoi(destinationPortRange[1]);

        if (sourcePortEnd - sourcePortStart !=
            destinationPortEnd - destinationPortStart) {
          LOG(FATAL) << "source/destination port range mismatch";
          exit(1);
        } else {
          int portRangeLength = sourcePortEnd - sourcePortStart + 1;
          for (int i = 0; i < portRangeLength; ++i) {
            pairs.push_back(
                make_pair(sourcePortStart + i, destinationPortStart + i));
          }
        }
      } else if (sourceDestination[0].find('-') != string::npos ||
                 sourceDestination[1].find('-') != string::npos) {
        LOG(FATAL) << "Invalid port range syntax: if source is range, "
                      "destination must be range";
      } else {
        int sourcePort = stoi(sourceDestination[0]);
        int destinationPort = stoi(sourceDestination[1]);
        pairs.push_back(make_pair(sourcePort, destinationPort));
      }
    } catch (const std::logic_error& lr) {
      LOG(FATAL) << "Logic error: " << lr.what();
      exit(1);
    }
  }
  return pairs;
}

void TerminalClient::run(const string& command, const string& tunnels,
                         const string& reverseTunnels) {
  console->setup();

  shared_ptr<TcpSocketHandler> socketHandler =
      static_pointer_cast<TcpSocketHandler>(globalClient->getSocketHandler());

  // Whether the TE should keep running.
  bool run = true;

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
  bool waitingOnKeepalive = false;

  if (command.length()) {
    LOG(INFO) << "Got command: " << command;
    et::TerminalBuffer tb;
    tb.set_buffer(command + "; exit\n");

    globalClient->writePacket(
        Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
  }

  try {
    if (tunnels.length()) {
      auto pairs = parseRangesToPairs(tunnels);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.set_sourceport(pair.first);
        pfsr.set_destinationport(pair.second);
        auto pfsresponse = portForwardHandler->createSource(pfsr);
        if (pfsresponse.has_error()) {
          throw std::runtime_error(pfsresponse.error());
        }
      }
    }
    if (reverseTunnels.length()) {
      auto pairs = parseRangesToPairs(reverseTunnels);
      for (auto& pair : pairs) {
        PortForwardSourceRequest pfsr;
        pfsr.set_sourceport(pair.first);
        pfsr.set_destinationport(pair.second);

        globalClient->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_SOURCE_REQUEST,
                   protoToString(pfsr)));
      }
    }
  } catch (const std::runtime_error& ex) {
    cerr << "Error establishing port forward: " << ex.what() << endl;
    LOG(FATAL) << "Error establishing port forward: " << ex.what();
  }

  TerminalInfo lastTerminalInfo;

  while (run && !globalClient->isShuttingDown()) {
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    int consoleFd = console->getFd();
    int maxfd = consoleFd;
    FD_SET(consoleFd, &rfd);
    int clientFd = globalClient->getSocketFd();
    if (clientFd > 0) {
      FD_SET(clientFd, &rfd);
      maxfd = max(maxfd, clientFd);
    }
    // TODO: set port forward sockets as well for performance reasons.
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);

    try {
      // Check for data to send.
      if (FD_ISSET(consoleFd, &rfd)) {
        // Read from stdin and write to our client that will then send it to the
        // server.
        VLOG(4) << "Got data from stdin";
        int rc = read(consoleFd, b, BUF_SIZE);
        FATAL_FAIL(rc);
        if (rc > 0) {
          // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " " <<
          // globalClient->getWriter()->getSequenceNumber();
          string s(b, rc);
          et::TerminalBuffer tb;
          tb.set_buffer(s);

          globalClient->writePacket(
              Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
          keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
        }
      }

      if (clientFd > 0 && FD_ISSET(clientFd, &rfd)) {
        VLOG(4) << "Cliendfd is selected";
        while (globalClient->hasData()) {
          VLOG(4) << "GlobalClient has data";
          Packet packet;
          if (!globalClient->read(&packet)) {
            break;
          }
          char packetType = packet.getHeader();
          if (packetType == et::TerminalPacketType::PORT_FORWARD_DATA ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_SOURCE_REQUEST ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_SOURCE_RESPONSE ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE) {
            keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
            VLOG(4) << "Got PF packet type " << packetType;
            portForwardHandler->handlePacket(packet, globalClient);
            continue;
          }
          switch (packetType) {
            case et::TerminalPacketType::TERMINAL_BUFFER: {
              VLOG(3) << "Got terminal buffer";
              // Read from the server and write to our fake terminal
              et::TerminalBuffer tb =
                  stringToProto<et::TerminalBuffer>(packet.getPayload());
              const string& s = tb.buffer();
              // VLOG(5) << "Got message: " << s;
              // VLOG(1) << "Got byte: " << int(b) << " " << char(b) << " " <<
              // globalClient->getReader()->getSequenceNumber();
              keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
              console->write(s);
              break;
            }
            case et::TerminalPacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              // This will fill up log file quickly but is helpful for debugging
              // latency issues.
              LOG(INFO) << "Got a keepalive";
              break;
            default:
              LOG(FATAL) << "Unknown packet type: " << int(packetType);
          }
        }
      }

      if (clientFd > 0 && keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          globalClient->closeSocketAndMaybeReconnect();
          waitingOnKeepalive = false;
        } else {
          LOG(INFO) << "Writing keepalive packet";
          globalClient->writePacket(Packet(TerminalPacketType::KEEP_ALIVE, ""));
          waitingOnKeepalive = true;
        }
      }
      if (clientFd < 0) {
        // We are disconnected, so stop waiting for keepalive.
        waitingOnKeepalive = false;
      }

      TerminalInfo ti = console->getTerminalInfo();

      if (ti != lastTerminalInfo) {
        LOG(INFO) << "Window size changed: " << ti.DebugString();
        lastTerminalInfo = ti;
        globalClient->writePacket(
            Packet(TerminalPacketType::TERMINAL_INFO, protoToString(ti)));
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardHandler->update(&requests, &dataToSend);
      for (auto& pfr : requests) {
        globalClient->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST,
                   protoToString(pfr)));
        VLOG(4) << "send PF request";
        keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
      }
      for (auto& pwd : dataToSend) {
        globalClient->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
        VLOG(4) << "send PF data";
        keepaliveTime = time(NULL) + CLIENT_KEEP_ALIVE_DURATION;
      }
    } catch (const runtime_error& re) {
      LOG(ERROR) << "Error: " << re.what();
      cout << "Connection closing because of error: " << re.what() << endl;
      run = false;
    }
  }
  globalClient.reset();
  LOG(INFO) << "Client derefernced";
  console->teardown();
  cout << "Session terminated" << endl;
}

int main(int argc, char** argv) {
  // Version string need to be set before GFLAGS parse arguments
  SetVersionString(string(ET_VERSION));

  // Setup easylogging configurations
  el::Configurations defaultConf = LogHandler::setupLogHandler(&argc, &argv);

  // GFLAGS parse command line arguments
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_logtostdout) {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "true");
  } else {
    defaultConf.setGlobally(el::ConfigurationType::ToStandardOutput, "false");
    // Redirect std streams to a file
    LogHandler::stderrToFile("/tmp/etclient");
  }

  // silent Flag, since etclient doesn't read /etc/et.cfg file
  if (FLAGS_silent) {
    defaultConf.setGlobally(el::ConfigurationType::Enabled, "false");
  }

  LogHandler::setupLogFile(&defaultConf,
                           "/tmp/etclient-%datetime{%Y-%M-%d_%H_%m_%s}.log");

  el::Loggers::reconfigureLogger("default", defaultConf);
  // set thread name
  el::Helpers::setThreadName("client-main");

  // Install log rotation callback
  el::Helpers::installPreRollOutCallback(LogHandler::rolloutHandler);

  // Override -h & --help
  for (int i = 1; i < argc; i++) {
    string s(argv[i]);
    if (s == "-h" || s == "--help") {
      cout << "et (options) [user@]hostname[:port]\n"
              "Options:\n"
              "-h Basic usage\n"
              "-p Port for etserver to run on.  Default: 2022\n"
              "-u Username to connect to ssh & ET\n"
              "-v=9 verbose log files\n"
              "-c Initial command to execute upon connecting\n"
              "-prefix Command prefix to launch etserver/etterminal on the "
              "server side\n"
              "-t Map local to remote TCP port (TCP Tunneling)\n"
              "   example: et -t=\"18000:8000\" hostname maps localhost:18000\n"
              "-rt Map remote to local TCP port (TCP Reverse Tunneling)\n"
              "   example: et -rt=\"18000:8000\" hostname maps hostname:18000\n"
              "to localhost:8000\n"
              "-jumphost Jumphost between localhost and destination\n"
              "-jport Port to connect on jumphost\n"
              "-x Flag to kill all sessions belongs to the user\n"
              "-logtostdout Sent log message to stdout\n"
              "-silent Disable all logs\n"
              "-noratelimit Disable rate limit"
           << endl;
      exit(1);
    }
  }

  GOOGLE_PROTOBUF_VERIFY_VERSION;
  srand(1);

  // Parse command-line argument
  if (argc > 1) {
    string arg = string(argv[1]);
    if (arg.find('@') != string::npos) {
      int i = arg.find('@');
      FLAGS_u = arg.substr(0, i);
      arg = arg.substr(i + 1);
    }
    if (arg.find(':') != string::npos) {
      int i = arg.find(':');
      FLAGS_port = stoi(arg.substr(i + 1));
      arg = arg.substr(0, i);
    }
    FLAGS_host = arg;
  }

  Options options = {
      NULL,  // username
      NULL,  // host
      NULL,  // sshdir
      NULL,  // knownhosts
      NULL,  // ProxyCommand
      NULL,  // ProxyJump
      0,     // timeout
      0,     // port
      0,     // StrictHostKeyChecking
      0,     // ssh2
      0,     // ssh1
      NULL,  // gss_server_identity
      NULL,  // gss_client_identity
      0      // gss_delegate_creds
  };

  char* home_dir = ssh_get_user_home_dir();
  string host_alias = FLAGS_host;
  ssh_options_set(&options, SSH_OPTIONS_HOST, FLAGS_host.c_str());
  // First parse user-specific ssh config, then system-wide config.
  parse_ssh_config_file(&options, string(home_dir) + USER_SSH_CONFIG_PATH);
  parse_ssh_config_file(&options, SYSTEM_SSH_CONFIG_PATH);
  LOG(INFO) << "Parsed ssh config file, connecting to " << options.host;
  FLAGS_host = string(options.host);

  // Parse username: cmdline > sshconfig > localuser
  if (FLAGS_u.empty()) {
    if (options.username) {
      FLAGS_u = string(options.username);
    } else {
      FLAGS_u = string(ssh_get_local_username());
    }
  }

  // Parse jumphost: cmd > sshconfig
  if (options.ProxyJump && FLAGS_jumphost.length() == 0) {
    string proxyjump = string(options.ProxyJump);
    size_t colonIndex = proxyjump.find(":");
    if (colonIndex != string::npos) {
      string userhostpair = proxyjump.substr(0, colonIndex);
      size_t atIndex = userhostpair.find("@");
      if (atIndex != string::npos) {
        FLAGS_jumphost = userhostpair.substr(atIndex + 1);
      }
    } else {
      FLAGS_jumphost = proxyjump;
    }
    LOG(INFO) << "ProxyJump found for dst in ssh config: " << proxyjump;
  }

  string idpasskeypair = SshSetupHandler::SetupSsh(
      FLAGS_u, FLAGS_host, host_alias, FLAGS_port, FLAGS_jumphost, FLAGS_jport,
      FLAGS_x, FLAGS_v, FLAGS_prefix, FLAGS_noratelimit);

  string id = "", passkey = "";
  // Trim whitespace
  idpasskeypair.erase(idpasskeypair.find_last_not_of(" \n\r\t") + 1);
  size_t slashIndex = idpasskeypair.find("/");
  if (slashIndex == string::npos) {
    LOG(FATAL) << "Invalid idPasskey id/key pair: " << idpasskeypair;
  } else {
    id = idpasskeypair.substr(0, slashIndex);
    passkey = idpasskeypair.substr(slashIndex + 1);
    LOG(INFO) << "ID PASSKEY: " << id << " " << passkey;
  }
  if (passkey.length() != 32) {
    LOG(FATAL) << "Invalid/missing passkey: " << passkey << " "
               << passkey.length();
  }
  bool is_jumphost = false;
  if (!FLAGS_jumphost.empty()) {
    is_jumphost = true;
    FLAGS_host = FLAGS_jumphost;
    FLAGS_port = FLAGS_jport;
  }
  SocketEndpoint socketEndpoint =
      SocketEndpoint(FLAGS_host, FLAGS_port, is_jumphost);
  shared_ptr<SocketHandler> clientSocket(new TcpSocketHandler());
  shared_ptr<Console> console(new PsuedoTerminalConsole());

  TerminalClient terminalClient =
      TerminalClient(clientSocket, socketEndpoint, id, passkey, console);
  terminalClient.run(FLAGS_c, FLAGS_t, FLAGS_rt);

  // Uninstall log rotation callback
  el::Helpers::uninstallPreRollOutCallback();
  return 0;
}