#!/bin/bash

g++ `sdl-config --cflags` -g *.cpp -pthread -lSDL -lSDL_net -lz -lGL -lGLU -o cppcraft
