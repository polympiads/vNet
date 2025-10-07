cmake_minimum_required(VERSION 3.28.0)
PROJECT(vNet)
SET(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++11")

ADD_SUBDIRECTORY(proto)
include_directories(${ProtobufIncludePath})