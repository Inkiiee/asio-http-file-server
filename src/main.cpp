#include <iostream>
#include <string>

#include "http_file_server.h"

int main(int argc, char* argv[]){
    if(argc < 3){
        std::cerr << "Usage: " << argv[0] << " <address> <port> [root_path]" << std::endl;
        return 1;
    }

    try{
        http_file_server::HttpFileServer server(argv[1], argv[2]);
        if(argc >= 4)
            server.set_root_path(argv[3]);
        server.start();
    }
    catch(const std::exception& e){
        std::cerr << "Exception in File Transfer Server: " << e.what() << std::endl;
    }
    return 0;
}