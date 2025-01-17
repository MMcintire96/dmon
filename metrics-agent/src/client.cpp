#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include "service-guide.grpc.pb.h"
#include "../include/networkMon.h"
#include "../include/systemMon.h"

#include "../include/json.hpp"
#include <fstream>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

using hostService::HostService;
using hostService::TransmitPacket;
using hostService::TransmitResponse;
using hostService::TransmitProcesses;
using hostService::TransmitProcessesResponse;
using json = nlohmann::json;

std::string CLIENT_VERSION = "1.0.1";

bool initalized = false;
std::string clientHostName;
std::string ipAddr;
std::string deviceID;
int responsetime;
NetworkMonitor networkMon;
SystemMonitor systemMon;

void initSystemMonitors() {
  networkMon = NetworkMonitor();
  systemMon = SystemMonitor();

  clientHostName = networkMon.getHostName();
  ipAddr = networkMon.getIPV4Addr();
  std::hash<std::string> str_hash;
  deviceID = std::to_string(str_hash(ipAddr));

  initalized = true;
}

TransmitPacket setStatic(TransmitPacket p) {
  p.set_hostname(clientHostName);
  p.set_ip(ipAddr);
  p.set_deviceid(deviceID);
  p.set_clientversion(CLIENT_VERSION);
  return p;
}

TransmitPacket setRAM(TransmitPacket p) {
  SystemMonitor::memoryStruct mem = systemMon.getMem();
  p.set_memoryused(mem.memUsed);
  p.set_memoryavailable(mem.freeMem);
  p.set_memorytotal(mem.totalMem);
  return p;
}

TransmitPacket setNetwork(TransmitPacket p) {
  NetworkMonitor::bandwidthStruct bStruct = networkMon.getBandwidth();
  p.set_inboundbandwithbytes(bStruct.r_bytes);
  p.set_outboundbandwithbytes(bStruct.t_bytes);
  p.set_inboundbandwithpackets(bStruct.r_packets);
  p.set_outboundbandwithpackets(bStruct.t_packets);
  return p;
}

TransmitPacket setSystemTelemetry(TransmitPacket p) {
  SystemMonitor::versionStruct vStruct = systemMon.getVersion();
  double cpu = systemMon.getCpu();
  p.set_cpuusage(cpu);
  p.set_version(vStruct.versionTag);
  p.set_uptime(systemMon.getUptime());
  return p;
}


TransmitPacket MakeTransmitPacket() {
  TransmitPacket p;

  p = setStatic(p);
  p = setRAM(p);
  p = setNetwork(p);
  p = setSystemTelemetry(p);

  return p;
}

TransmitProcesses MakeTransmitProcessesPackets() {
  TransmitProcesses p;
  std::string processes = *systemMon.getProcesses();
  google::protobuf::RepeatedField<std::string> procs;
  procs.Reserve(10);
  *procs.mutable_data() = {processes.begin(), processes.end()};
  return p;
}

//both of theses streaming clients should be abstracted.
class ProcessMonitor {
  public:
    ProcessMonitor(std::shared_ptr<Channel> channel)
      : stub_(HostService::NewStub(channel)) {}

    void StreamProcesses() {
      ClientContext context;
      std::shared_ptr<ClientReaderWriter<TransmitProcesses, TransmitProcessesResponse> > stream(
          stub_->Processes(&context));

      std::thread writer([stream]() {
              std::vector<TransmitProcesses> notes{
              MakeTransmitProcessesPackets(),
          };

          for (const TransmitProcesses& note : notes) {
            stream->Write(note);
          }

          stream->WritesDone();
      });
      writer.join();

      TransmitProcessesResponse server_note;
      while (stream->Read(&server_note))
        responsetime = server_note.frequencyadjustment();

      Status status = stream->Finish();
      if (!status.ok()) {
        std::cout << "Stream rpc failed." << std::endl;
      }
    }

  private:
    std::unique_ptr<HostService::Stub> stub_;
};


class ClientMonitor {
  public:
    ClientMonitor(std::shared_ptr<Channel> channel)
      : stub_(HostService::NewStub(channel)) {}

    void Stream() {
      ClientContext context;
      std::shared_ptr<ClientReaderWriter<TransmitPacket, TransmitResponse> > stream(
          stub_->Transmit(&context));
      sendPackets(stream);
      readResponse(stream);
      handleStatus(stream);
    }



    void handleStatus(std::shared_ptr<ClientReaderWriter<TransmitPacket, TransmitResponse>> stream) {
      Status status = stream->Finish();
      if (!status.ok()) {
        std::cout << "Stream rpc failed." << std::endl;
      }
    }


    void sendPackets(std::shared_ptr<ClientReaderWriter<TransmitPacket, TransmitResponse>> stream) {
      std::thread writer([stream]() {
              std::vector<TransmitPacket> notes{
              MakeTransmitPacket(),
          };

          for (const TransmitPacket& note : notes) {
            stream->Write(note);
          }

          stream->WritesDone();
      });
      writer.join();
    }


    void readResponse(std::shared_ptr<ClientReaderWriter<TransmitPacket, TransmitResponse>> stream) {
      TransmitResponse server_note;
      while (stream->Read(&server_note)) {
        if (server_note.didinsert() == 1) {
          std::cout << "Message insertion successful" << std::endl;
          responsetime = server_note.frequencyadjustment();
        } else {
          std::cout << "Message failed insertion (check that elastic search is running)" << std::endl;
          responsetime = server_note.frequencyadjustment();
        }
      }
    }


  private:
    std::unique_ptr<HostService::Stub> stub_;
};




int main(int argc, char** argv) {
  responsetime = 2000;
  std::string hostIP = "localhost:50486";
  if (argv[1] != NULL)
    hostIP = argv[1];


  initSystemMonitors();
  if (initalized == false)
    std::cout << "System Monitor failed to Initalize" << std::endl;

  ClientMonitor guide(grpc::CreateChannel(hostIP, grpc::InsecureChannelCredentials()));
  std::cout << "Client Connecting to " << hostIP << std::endl;
  ProcessMonitor pmon(grpc::CreateChannel(hostIP, grpc::InsecureChannelCredentials()));

  while (true) {
    guide.Stream();
    pmon.StreamProcesses();
    std::this_thread::sleep_for(std::chrono::milliseconds(responsetime));
  }
}
