﻿#include "plInternal.h"
#include "plNetwork.h"
#include "plCommunicator.h"
#include "PatchLibrary.h"

plCommunicator::plCommunicator()
    : m_running(false)
    , m_port(0)
    , m_thread(nullptr)
{
}

plCommunicator::~plCommunicator()
{
    stop();
}

bool plCommunicator::run(uint16_t port)
{
    if(m_running) { return false; }

    m_port = port;
    m_running = true;
    m_thread = new std::thread([&](){
        bool r = plRunTCPServer(m_port, [&](plTCPSocket &client){
            plProtocolSocket s(client.getHandle(), false);
            return onAccept(s);
        });
        m_running = false;
    });
    return true;
}

void plCommunicator::stop()
{
    if(!m_running) { return; }
    // todo
}

bool plCommunicator::onAccept(plProtocolSocket &client)
{
    plString command;
    client.read(command);
    if(strncmp(&command[0], "patch ", 6)==0) {
        plString dllpath = &command[6];
        PatchLibraryA(dllpath.c_str());
    }
    return true;
}
