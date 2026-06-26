#include <asio.hpp>
#include <memory>
#include <cstdint>
#include <array>
#include <vector>
#include <iostream>

#include "udp_broadcast_server.h"

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::io_context;
using asio::ip::udp;
using asio::as_tuple;
using namespace udp_broadcast;

UdpBroadcastServer::UdpBroadcastServer(
    const std::string& port, 
    const std::string& target_server_address, 
    const std::string& target_server_port,
    const std::string& id
) : socket_(io_context_), recv_buffer_(2048), target_server_address_(target_server_address), target_server_port_(target_server_port), id_(id) {
    // 브로드캐스트 수신을 위해서는 유니캐스트 주소가 아닌 INADDR_ANY(0.0.0.0)에 바인딩해야 한다.
    udp::endpoint listen_endpoint(asio::ip::address_v4::any(), std::stoi(port));
    socket_.open(listen_endpoint.protocol());
    socket_.bind(listen_endpoint);
    socket_.set_option(udp::socket::broadcast(true));
    
    std::cout << "[UDP Server] Listening on port " << port << std::endl;
    std::cout << "[UDP Server] Server ID: " << id << std::endl;
}

UdpBroadcastServer::~UdpBroadcastServer(){
    stop();
}

awaitable<void> UdpBroadcastServer::start_receive(){
    try{
        for(;;){
            auto [e, n] = co_await socket_.async_receive_from(asio::buffer(recv_buffer_), sender_endpoint_, as_tuple);
            if(e)
                throw std::runtime_error("Failed to receive data");
            
            std::cout << "Received " << n << " bytes from " << sender_endpoint_ << std::endl;
            std::vector<uint8_t> data(recv_buffer_.begin(), recv_buffer_.begin() + n);
            co_await handle_receive(data, sender_endpoint_);
        }
    }
    catch(std::exception& e){
        stop();
        std::cerr << "Exception: " << e.what() << "\n";
    }
}

awaitable<void> UdpBroadcastServer::handle_receive(const std::vector<uint8_t>& data, const udp::endpoint& sender_endpoint){
    if(data.size() < 8 || data[0] != 'U' || data[1] != 'S'){
        std::cerr << "Invalid packet received from " << sender_endpoint << std::endl;
        co_return;
    }
    PacketType packet_type = *reinterpret_cast<const PacketType*>(data.data() + 2);
    if(packet_type != PacketType::REQUEST_SERVER_INFO){
        std::cerr << "Unknown packet type received from " << sender_endpoint << std::endl;
        co_await send_packet(create_packet(PacketType::UNKNOWN_REQUEST, "Unknown Request. Maybe a not supported packet type."), sender_endpoint);
        co_return;
    }

    uint32_t packet_length = *reinterpret_cast<const uint32_t*>(data.data() + 4);
    if(packet_length > 2040){
        std::cerr << "Packet length exceeds buffer size from " << sender_endpoint << std::endl;
        co_await send_packet(create_packet(PacketType::SERVER_INFO_ERROR, "Packet length exceeds buffer size."), sender_endpoint);
        co_return;
    }
    if(data.size() < 8 + packet_length){
        std::cerr << "Incomplete packet received from " << sender_endpoint << std::endl;
        co_await send_packet(create_packet(PacketType::SERVER_INFO_ERROR, "Incomplete packet received."), sender_endpoint);
        co_return;
    }

    std::vector<uint8_t> payload(data.begin() + 8, data.begin() + 8 + packet_length);
    if(payload[0] != 'I' || payload[1] != 'K'){
        std::cerr << "Invalid payload received from " << sender_endpoint << std::endl;
        co_await send_packet(create_packet(PacketType::SERVER_INFO_ERROR, "Invalid payload received."), sender_endpoint);
        co_return;
    }

    std::string id(payload.begin() + 2, payload.end());
    if(id != id_){
        std::cerr << "ID mismatch received from " << sender_endpoint << std::endl;
        co_return;
    }

    std::cout << "REQUEST_SERVER_INFO received from " << sender_endpoint << std::endl;

    std::vector<uint8_t> response_payload;
    response_payload.push_back('I');
    response_payload.push_back('K');
    uint32_t server_info_length = target_server_address_.size() + target_server_port_.size() + id_.size() + 2;
    response_payload.insert(response_payload.end(), reinterpret_cast<uint8_t*>(&server_info_length), reinterpret_cast<uint8_t*>(&server_info_length) + sizeof(server_info_length));
    
    response_payload.insert(response_payload.end(), id_.begin(), id_.end());
    response_payload.push_back('\0');
    response_payload.insert(response_payload.end(), target_server_address_.begin(), target_server_address_.end());
    response_payload.push_back('\0');
    response_payload.insert(response_payload.end(), target_server_port_.begin(), target_server_port_.end());

    co_await send_packet(create_packet(PacketType::SERVER_INFO_RESPONSE, response_payload), sender_endpoint);
}

awaitable<void> UdpBroadcastServer::send_packet(std::vector<uint8_t> data, const udp::endpoint& target_endpoint){
    try{
        auto [e, n] = co_await socket_.async_send_to(asio::buffer(data), target_endpoint, as_tuple);
        if(e)
            throw std::runtime_error("Failed to send data");
        
        std::cout << "Sent " << n << " bytes to " << target_endpoint << std::endl;
    }
    catch(std::exception& e){
        std::cerr << "Exception in send_packet: " << e.what() << "\n";
    }
}

std::vector<uint8_t> UdpBroadcastServer::create_packet(PacketType packet_type, const std::vector<uint8_t>& payload){
    std::vector<uint8_t> packet(8 + payload.size());
    packet[0] = 'U';
    packet[1] = 'S';
    *reinterpret_cast<uint16_t*>(packet.data() + 2) = static_cast<uint16_t>(packet_type);
    *reinterpret_cast<uint32_t*>(packet.data() + 4) = static_cast<uint32_t>(payload.size());
    std::copy(payload.begin(), payload.end(), packet.begin() + 8);
    return packet;
}

std::vector<uint8_t> UdpBroadcastServer::create_packet(PacketType packet_type, const std::string& payload_str){
    std::vector<uint8_t> payload(payload_str.begin(), payload_str.end());
    return create_packet(packet_type, payload);
}

void UdpBroadcastServer::start(){
    co_spawn(io_context_, start_receive(), detached);
    io_context_.run();
}

void UdpBroadcastServer::stop(){
    io_context_.stop();
    socket_.close();
}