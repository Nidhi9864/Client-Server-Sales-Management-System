# Client-Server-Sales-Management-System

# Branch Sales Aggregator System (CSP Project)

## Overview
The **Branch Sales Aggregator System** is a distributed client–server application built using **C programming and socket-based inter-process communication**.  
It demonstrates **Computer System Programming (CSP) concepts** such as networking, file handling, synchronization, concurrency control, and fault tolerance.

Each branch runs its own server that processes local sales data stored in a CSV file.  
A central **Aggregator** connects to these branch servers, collects summarized data, and safely updates a **main consolidated CSV file**.

---

## System Architecture

- **Branch Server**
  - Runs independently for each branch.
  - Reads branch-specific sales data from a CSV file.
  - Computes total sales (subtotal) and number of records.
  - Sends the summary to the Aggregator on request.

- **Main Aggregator**
  - Acts as a client.
  - Connects to multiple branch servers simultaneously.
  - Requests sales summaries.
  - Aggregates responses and updates the main CSV file atomically.

---

## Key Features

### 1. Distributed Processing
- Each branch processes its own data independently.
- Aggregation happens centrally without sharing raw data.

### 2. Socket-Based Communication
- Uses **TCP sockets** (`SOCK_STREAM`) for reliable communication.
- Supports multiple branch servers running on different ports or machines.

### 3. Simple Text-Based Protocol
- Aggregator sends:
REQUEST

- Branch server responds:


BRANCH_ID: <id>
RECORDS: <count>
SUBTOTAL: <amount>
END


### 4. Fault Tolerance
- Aggregator continues execution even if one branch is unreachable.
- Uses timeout (`select()`) to avoid blocking indefinitely.

### 5. Safe and Atomic File Updates
- Uses:
- `flock()` for file locking
- Temporary file + `rename()` for atomic writes
- `fsync()` to ensure data is written to disk
- Prevents data corruption during concurrent access.

### 6. Robust I/O Handling
- Handles partial `send()` and interrupted system calls (`EINTR`).
- Ensures complete message transmission.

---

## Technologies and Concepts Used

- **C Programming**
- **POSIX System Calls**
- **TCP/IP Networking**
- **Socket Programming**
- **File Handling**
- **File Locking (`flock`)**
- **Concurrency Control**
- **Timeout Handling (`select`)**
- **CSV Parsing**
- **Atomic File Operations**

---

## File Structure

1. branch_server.c # Branch server implementation
2. main_aggregator.c # Central aggregator
3. branchA.csv # Sample branch A data
4. branchB.csv # Sample branch B data
5. main.csv # Aggregated output
