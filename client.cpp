/**
 *  Copyright 2011 by Benjamin J. Land (a.k.a. BenLand100)
 *
 *  This file is part of the CppCraft.
 *
 *  CppCraft is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  CppCraft is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CppCraft. If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include "client.h"
#include "packets.h"
#include "render.h"
#include "tools.h"
#include <cmath>

Client::Client() {
    digging = false;
    target = beingdug = NULL;
    forwards = sideways = 0;
    connected = false;
    socket = NULL;
    doPhysics = false;
    physics = NULL;
    doPackets = false;
    packets = NULL;
    us = NULL;
    usLock = false;
    usLock = SDL_CreateMutex();
}

Client::~Client() {
    disconnect();
    SDL_DestroyMutex(usLock);
}

void Client::lockUs() {
    SDL_mutexP(usLock);
}

void Client::unlockUs() {
    SDL_mutexV(usLock);
}
        
bool Client::connect(char *host, int port) {
    if (!socket) {
        IPaddress ip;
        if (SDLNet_ResolveHost(&ip, host, port) < 0) {
	        std::cout << "Error resolving host: " << SDLNet_GetError() << '\n';
	        return false;
        }
        if (!(socket = SDLNet_TCP_Open(&ip))) {
	        std::cout << "Error connecting: " << SDLNet_GetError() << '\n';
	        socket = NULL;
	        return false;
        }
        connected = true;
        doPackets = true;
        packets = SDL_CreateThread((int (*)(void*))packets_thread, this);
        return true;
    } else {
        return false;
    }
}

int physics_thread(Client *client) {
    double diginc = 0.0;
    while (client->doPhysics) {
        SDL_Delay(50);
        double dt = 0.05;
        
        client->lockUs();
        client->retarget();
        if (client->digging) {
            if (client->target) {
                if (client->beingdug != client->target) {
                    client->digStatus = 0.0;
                    diginc = incPerTick(-1,client->target->type,false,client->onGround); //TODO should use the held tool and underwater status
                    client->beingdug = client->target;
                    send_player_digging(client->socket,0,client->targetx,client->targety,client->targetz,client->targetface);
                } else {
                    client->digStatus += diginc;
                    if (client->digStatus >= 1.0) { //Should use the pause required by the block
                        client->beingdug = NULL;
                        client->digStatus = 0.0;
                        send_player_digging(client->socket,2,client->targetx,client->targety,client->targetz,client->targetface);
                        std::cout << "Dug: " << std::dec << client->targetx << ' ' << client->targety << ' ' << client->targetz << ' ' << client->targetface << '\n';
                    }
                }
            } else {
                client->beingdug = NULL;
            }
        }
        
        if (client->forwards || client->sideways) {
            double fx = cos((client->us->yaw)/180.0*3.14159);
            double fz = sin((client->us->yaw)/180.0*3.14159);
            client->us->vx = -fz*client->sideways+fx*client->forwards;
            client->us->vz = fx*client->sideways+fz*client->forwards;
        } else {
            client->us->vx = 0;
            client->us->vz = 0;
        }
        
        client->us->vy -= 30*dt;
        
        if (client->us->vx > 5.0) client->us->vx = 5.0;
        if (client->us->vy > 10.0) client->us->vy = 5.0;
        if (client->us->vz > 5.0) client->us->vz = 5.0;
        
        //bounding box in block coords
        int sx,sy,sz,ex,ey,ez;
        client->us->boundingBox(sx,sy,sz,ex,ey,ez);
        
        if (client->us->vx != 0) {
            client->us->x += client->us->vx*dt;
            int x = floor(client->us->x + (client->us->vx < 0 ? -1.0 : 1.0)*client->us->apothem); 
            if (client->world.containsSolid(x,sy,sz,x,ey,ez)) {
                client->us->x = x + ((client->us->vx > 0) ? -client->us->apothem : 1+client->us->apothem) * 1.001; //adjust slightly
                client->us->vx = 0;
            }
        }
        if (client->us->vz != 0) {
            client->us->z += client->us->vz*dt;
            int z = floor(client->us->z + (client->us->vz < 0 ? -1.0 : 1.0)*client->us->apothem); 
            if (client->world.containsSolid(sx,sy,z,ex,ey,z)) {
                client->us->z = z + ((client->us->vz > 0) ? -client->us->apothem : 1+client->us->apothem) * 1.001; //adjust slightly
                client->us->vz = 0;
            }
        }
        client->onGround = false;
        if (client->us->vy != 0) {
            client->us->y += client->us->vy*dt;
            //position y at our feet or head relative to velocity
            int y = floor(client->us->y + ((client->us->vy > 0) ? 1.74 : 0)); 
            if (client->world.containsSolid(sx,y,sz,ex,y,ez)) {
                client->us->y = y + ((client->us->vy > 0) ? -1.74 : 1) * 1.001; //adjust slightly above/below
                if (client->us->vy < 0) client->onGround = true;
                client->us->vy = 0;
            }
        }
        
        client->sendPos();
        client->unlockUs();
    }
    return 0;
}

void Client::packet(p_generic *p) {
    switch (p->id) {
        case 0x00:
            send_keep_alive(socket);
            break;
        case 0x01:
            std::cout << "Logged In! EID: " << std::dec <<((p_login_request_stc*)p)->EntityID << '\n';
            break;
        case 0x02:
            send_login_request_cts(socket,14,us->name,0,0);
            break;
        case 0x03:
            std::cout << "Chat: " << ((p_chat_message*)p)->Message << '\n';
            break;
        case 0x08:
            if (((p_update_health*)p)->Health <= 0) {
                lockUs();
                std::cout << "We died, respawning...\n";
                send_respawn(socket,0);
                unlockUs();
            }
            break;
        case 0x0D:
            {
                p_player_position_and_look_stc *pos = (p_player_position_and_look_stc*)p;
                lockUs();
                onGround = pos->OnGround;
                us->x = pos->X;
                us->y = pos->Y;
                us->z = pos->Z;
                us->height = pos->Stance - pos->Y;
                us->pitch = pos->Pitch;
                us->yaw = pos->Yaw;
                sendPos();
                Chunk *c = world.getChunk(us->x,us->y,us->z); if (c) c->markDirty();
                unlockUs();
            }
            if (!physics) {
                doPhysics = true;
                physics = SDL_CreateThread((int (*)(void*))physics_thread, this);
            }
            break;
        case 0x18:
            break;
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x26:
            break; //ignore entity motion stuff for now
        case 0x32: 
            {
                p_prechunk *prechunk = (p_prechunk*)p;
                if (!prechunk->Mode) world.deleteChunk(prechunk->X,0,prechunk->Z);
            }
            break;
        case 0x33:
            {
                p_map_chunk *update = (p_map_chunk*)p;
                bool res = world.updateChunk(update->X,update->Y,update->Z,update->SizeX,update->SizeY,update->SizeZ,update->CompressedSize,update->CompressedData);
                if (!res) {
                    std::cout << "Error performing chunk update\n";
                    disconnect();
                }
            }
            break;
        case 0x34:
            {
                p_multi_block_change *mbc = (p_multi_block_change*)p;
                bool res = world.updateChunk(mbc->ChunkX,0,mbc->ChunkZ,mbc->ArraySize,mbc->CoordinateArray,mbc->TypeArray,mbc->MetadataArray);
                //Ignore if the chunk is not currently loaded...?
            }
            break;
        case 0x35:
            {
                p_block_change *change = (p_block_change*)p;
                Block *b = world.getBlock(change->X,change->Y,change->Z);
                if (b) { //Ignore if the chunk is not currently loaded...?
                    b->type = change->Type;
                    b->meta = change->Metadata;
                    world.updateLighting(change->X,change->Y,change->Z); 
                }
            }
            break;
        case 0xFF:
            std::cout << "KICK: " << ((p_kick*)p)->Message << '\n';
            disconnect();
            break;
        default:
            std::cout << "Unhandled Packet: 0x" << std::hex << (int)p->id << '\n';
    }
}

void Client::disconnect() {
    doPackets = false;
    doPhysics = false;
    if (physics) SDL_WaitThread(physics, NULL);
    if (packets) SDL_WaitThread(packets, NULL);
    physics = NULL;
    packets = NULL;
    connected = false;
    if (socket) SDLNet_TCP_Close(socket);
    socket = NULL;
    if (us) delete us;
    us = NULL;
    world.clearChunks();
}

bool Client::login(char *username) {
    if (!us) {
        us = new Player(username);
        send_handshake_cts(socket,us->name);
        return true;
    }
}

void Client::startDigging() {
    lockUs();
    beingdug = NULL;
    digging = true;
    unlockUs();
}

void Client::stopDigging() {
    lockUs();
    digging = false;
    beingdug = NULL;
    unlockUs();
}

void Client::placeHeld() {
    //TODO formulate and send the block place packet
    std::cout << "Place here...\n";
}

void Client::jump() {
    lockUs();
    if (onGround) {
        us->vy = 10.0;
    }
    unlockUs();
}

void Client::relLook(double dpitch, double dyaw) {
    lockUs();
    us->pitch += dpitch;
    us->yaw += dyaw;
    if (us->pitch > 90.0) us->pitch = 90.0;
    if (us->pitch < -90.0) us->pitch = -90.0;
    if (us->yaw > 360.0 || us->yaw < 0) us->yaw = fmod(us->yaw,360.0);
    retarget();
    unlockUs();
}

void Client::setMotion(double forwards, double sideways) {
    lockUs();
    this->forwards = forwards;
    this->sideways = sideways;
    unlockUs();
}

Block* Client::getTarget(int &x, int &y, int &z, int &face) {
    x = targetx;
    y = targety;
    z = targetz;
    face = targetface;
    return target;
}

bool Client::running() {
    return connected;
}

void Client::sendPos() {
    lockUs();
    send_player_position_and_look_cts(socket,us->x,us->y,us->height+us->y,us->z,us->yaw,us->pitch,onGround);
    unlockUs();
}

void Client::retarget() {
    target = world.projectToBlock(us->x,us->y+us->height,us->z,us->pitch,us->yaw,targetx,targety,targetz,targetface);
}

void Client::init() {
    if (SDLNet_Init() < 0) {
        std::cout << "Failed to init SDLNet: " << SDLNet_GetError() <<'\n';
        exit(1);
    }
}

void Client::quit() {
    SDLNet_Quit();
}

int main(int argc, char** argv) {

    Client::init();
    initRender();
    Client *c = new Client();
    if (c->connect((char*)"localhost")) {
        if (c->login((char*)"YourMom")) {
            while (c->running()) {
                SDL_Delay(10);
                renderWorld(c);
                processEvents(c);
            }
        }
    }
    delete c;
    quitRender();
    Client::quit();
    
    std::cout << "Finished!\n";
    
    return 0;
}
