/* Copyright (c) 2011, NEC Europe Ltd, Consorzio Nazionale
 * Interuniversitario per le Telecomunicazioni. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the names of NEC Europe Ltd, Consorzio Nazionale
 *      Interuniversitario per le Telecomunicazioni nor the names of its contributors
 *      may be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL COPYRIGHT
 * HOLDERBE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
 */


/**
 * <blockinfo type="SerSource" invocation="direct" thread_exclusive="True" thread_safe="False">
 *  <humandesc>
 *  Receives data from the network and de-serialize it in a message.
 *  </humandesc>
 *
 *   <shortdesc>Import Blockmon internal messages from TCP session</shortdesc>
 *
 *   <gates>
 *     <gate type="input" name="in_msg" msg_type="Msg" m_start="0" m_end="0" />
 *   </gates>
 *
 *   <paramsschema>
 *    element params {
 *       element collect {
 *           attribute port {xsd:integer}?
 *           attribute msgtype {text},
 *       }
 *    }
 *   </paramsschema>
 *
 *   <paramsexample>
 *     <params>
 *       <collect portt="113" msgtype="Message"/>
 *     </params>
 *   </paramsexample>
 *
 *   <variables>
 *   </variables>
 *
 * </blockinfo>
 */

#include <boost/lexical_cast.hpp>
#include <Block.hpp>
#include <BlockFactory.hpp>
#include <MessageFactory.hpp>
#include <Serializer.hpp>

#include <stdio.h>
#include <string.h>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CLIENT_SOCKET 20
using namespace pugi;

namespace blockmon
{

    class SerSource: public Block {
        
    public:

        /**
         * @brief Constructor
         * @param name The name of the source block
         * @param invocation Invocation type of the block (ignored)
         */
        SerSource(const std::string &name, invocation_type invocation)
            :Block(name, invocation_type::Async),
            m_gate_id(register_output_gate("source_out")),
            sock(-1),
            sock_conn(-1),
            msg_prototype (NULL)
        {
            if (invocation != invocation_type::Async) {
                blocklog("SerSource must be Async, ignoring configuration", log_warning);
            }
        }

        /**
        * Expose ability to send a record (so the Receiver subclass can use it)
        *
        * Client code should not call this method.
        */

        void send_record(std::shared_ptr<const Msg>&& msg)
        {
            send_out_through(std::move(msg), m_gate_id);
        }

        /**
         * Configure the block given an XML element containing configuration.
         * Called before the block will begin receiving messages.
         *
         * @param xmlnode the <params> XML element containing block parameters
         */
        virtual void _configure(const xml_node& xmlnode)
        {
            xml_node domspec, inspec;

            if ((inspec = xmlnode.child("collect"))) {
                std::string cfgport = inspec.attribute("port").value();
                if (cfgport.empty()) {
                    throw std::runtime_error("Source specification incomplete");
                }
                port = atoi(cfgport.c_str());
                if(port < 0 || port > 65535) {
                    throw std::runtime_error("Invalid port");
                }

                std::string msgtype = inspec.attribute("msgtype").value();
                if (msgtype.empty()) {
                    throw std::runtime_error("Message type specification incomplete");
                }

                msg_prototype = MessageFactory::instantiate(msgtype);
                if (!msg_prototype) {
                    throw std::runtime_error(std::string(msgtype).append(": message type not supported"));
                }

            } else {
                throw std::runtime_error("No collector or file specification");
            }
        }

        virtual void _do_async()
        {
            if(sock == -1) {
                local.sin_family = AF_INET;
                local.sin_port = htons(port);
                local.sin_addr.s_addr = INADDR_ANY;
                sock = socket(AF_INET, SOCK_STREAM, 0);

                if( sock == -1 ) {
                    std::cerr << "Cannot create socket: " << strerror(errno) << std::endl;
                    return;
                }

                if( bind(sock, (struct sockaddr*) &local, sizeof(local)) == -1 ) {
                    std::cerr << "Cannot bind socket: " << strerror(errno) << std::endl;
                    return;
                }

                if( listen(sock, 10) == -1 ) {
                    std::cerr << "Cannot listen on the sock: " << strerror(errno) << std::endl;
                    return;
                }
            }

            struct sockaddr_in clientaddr;
            fd_set master, read_fds;
            FD_ZERO(&master);
            FD_ZERO(&read_fds);

            while(1) {
                // Consume data in the buffer. Then accepts new connection or get data from the network
                int consumed = 0;
                for(std::vector<Serializer *>::iterator it = sers.begin(); it < sers.end(); it++) {
                    Serializer *ser = *it;
                    if(ser->get_len() >= 2) {
                        int msg_len = ser->read_int16();
                        if(ser->get_len() >= msg_len) {
                            // Remove msg length: this should be moved into serializer
                            ser->consume(2);
                            this->send_record(msg_prototype->build_same(ser));
                            ser->next();
                            consumed = 1;
                        }
                    }
                }
                
                // If we consumed something then try consuming more, otherwise get data from the network
                if(consumed > 0)
                    continue;

                FD_ZERO(&read_fds);
                FD_SET(sock, &read_fds);
                // Add client socket
                for(std::vector<int>::iterator it = client_socket.begin(); it != client_socket.end(); it++) {
                    FD_SET(*it, &read_fds);
                }
                if(select(MAX_CLIENT_SOCKET+2, &read_fds, NULL, NULL, NULL) == -1) {
                    continue;
                }

                // New incoming connection
                if(FD_ISSET(sock, &read_fds)) {
                    int addrlen = sizeof(clientaddr);
                    int newfd;
                    if((newfd = accept(sock, (struct sockaddr *)&clientaddr, (socklen_t *) &addrlen)) == -1) {
                            std::cerr << "Error accepting new connection from "<< inet_ntoa(clientaddr.sin_addr) << std::endl;
                    } else if(client_socket.size() == MAX_CLIENT_SOCKET) {
                        std::cerr << "Reached maximum number of allowed connection" << std::endl;
                    } else {
                        client_socket.push_back(newfd);
                        Serializer *ser = new Serializer();
                        ser->empty();
                        sers.push_back(ser);
                        std::cerr << "New connection from "<< inet_ntoa(clientaddr.sin_addr) << std::endl;
                    }
                }

                for(unsigned int i = 0; i < client_socket.size(); i++) {
                    int s = client_socket[i];
                    // New connection is available
                    if (FD_ISSET(s, &read_fds)) {
                        #define DEFAULT_RECV_SIZE 2000
                        char tmpstorage[DEFAULT_RECV_SIZE];
                        int retc;
                        if((retc = recv(s, tmpstorage, sizeof(tmpstorage), 0)) <= 0) {
                            if(retc == 0) {
                                continue;
                            } else {
                                std::cerr << "Unexpected error: " << strerror(errno) << std::endl;
                                close(s);
                                continue;
                            }
                        }
                        sers[i]->add_raw_data( tmpstorage, retc );
                    }
                }
            }
        }
    
     private:
  
        int m_gate_id;
        int port;
        int sock, sock_conn;
        struct sockaddr_in local;
        struct sockaddr_in remote;
//         Msg *msg_prototype;
        std::shared_ptr<Msg> msg_prototype;
        std::vector<int> client_socket;
        std::vector<Serializer *> sers;
        
    
     };

     REGISTER_BLOCK(SerSource,"SerSource")
}
