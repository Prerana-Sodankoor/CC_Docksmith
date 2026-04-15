# Docksmith

Docksmith is a lightweight containerization and image management project designed to streamline image builds, caching, runtime execution, and dashboard-based monitoring. The project is structured to provide modular components for build orchestration, cache handling, runtime control, and sample application deployment.

---

## Overview

Docksmith is built with a modular architecture that separates core container workflow responsibilities into dedicated components:

* **Build and image management** through the main engine
* **Layer caching** for faster rebuilds and reduced redundancy
* **Runtime execution** for launching and managing containers
* **CLI support** for command-line interactions
* **Dashboard integration** for visualization and monitoring
* **Testing and sample deployment** utilities

This structure makes Docksmith suitable for research, educational use, and lightweight container workflow experimentation.

---

## Project Structure

```text
Docksmith/
├── cache/               # Build cache logic and storage handling
├── cli/                 # Command-line interface utilities
├── dashboard/           # Dashboard frontend/backend components
├── engine/              # Core build and orchestration engine
├── files/               # File handling and supporting assets
├── runtime/             # Container runtime execution logic
├── sample_app/          # Example application for testing builds
├── test_server/         # Test server environment
│   ├── Docksmithfile
│   └── server.sh
├── .gitignore
├── Docksmithfile        # Main build definition file
├── Makefile             # Build and automation commands
├── demo_cache.sh        # Cache demonstration script
├── docksmith            # Main executable / launcher
├── docksmith_gc.sh      # Garbage collection / cleanup script
├── main.c               # Core C source entry point
├── run_dashboard.sh     # Launch dashboard
├── run_live_demo.sh     # Run live demonstration
├── sample_app.sh        # Run sample application
└── setup_alpine.sh      # Alpine environment setup
```

---

## Core Components

### 1. Engine

The `engine/` module contains the primary logic responsible for:

* Parsing `Docksmithfile`
* Processing build instructions
* Managing layer execution
* Handling dependency order

### 2. Cache

The `cache/` directory manages build layer caching to improve performance.

Key responsibilities include:

* Layer hash generation
* Cache lookup
* Cache reuse
* Cache invalidation
* Optional garbage collection

### 3. Runtime

The `runtime/` module is responsible for executing built images and managing isolated environments.

### 4. CLI

The `cli/` module provides terminal-based commands for:

* Building images
* Running containers
* Cleaning cache
* Viewing logs

### 5. Dashboard

The `dashboard/` component provides a monitoring interface for:

* Build status
* Cache hits/misses
* Runtime state
* Logs and metrics

---

## Getting Started

### Prerequisites

Ensure the following tools are installed:

* GCC / Clang
* Make
* Bash
* Linux environment (recommended)

Optional:

* Docker-compatible environment for comparison/testing
* OpenSSL libraries if hashing is used

---

## Build Instructions

Compile the project using:

```bash
make
```

Or manually:

```bash
gcc main.c -o docksmith
```

---

## Usage

### Run Main Executable

```bash
./docksmith
```

### Run Dashboard

```bash
./run_dashboard.sh
```

### Run Live Demo

```bash
./run_live_demo.sh
```

---

## Docksmithfile

The `Docksmithfile` defines build instructions similar to container image definition files.

Example:

```dockerfile
FROM alpine
COPY . /app
RUN chmod +x /app/server.sh
CMD ["/app/server.sh"]
```

---

## Contributions:

PES2UG23CS450: Priyanka Satish : cli module 
PES2UG23CS417: Praagnya Parimi : Runtime module 
PES2UG23CS446: Prerana Sodankoor : cache module
PES2UG23CS451: Priyanka Shankar : engine module 
