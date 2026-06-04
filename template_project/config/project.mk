# Project Configuration Header

## Version
PROJECT_VERSION := 1.0.0
PROJECT_NAME := sample_project

## Build Output Directory
BUILD_DIR := build

## Target Hardware (modify as needed)
TARGET_DEVICE := esp32

## Compiler Flags
EXTRA_CFLAGS := -Wall -Wextra -Wpedantic
EXTRA_CXXFLAGS := -Wall -Wextra -Wpedantic

## Flash Parameters
FLASH_FREQ := 40m
FLASH_MODE := dio
FLASH_SIZE := detect
