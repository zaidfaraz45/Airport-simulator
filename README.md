# Airport Runway Simulation (Multithreaded C Program)

## Overview

This project simulates an airport runway management system using **C, POSIX threads, mutexes, semaphores, and ncurses UI**.

It demonstrates operating system concepts like:
- Thread scheduling
- Mutual exclusion (mutex)
- Synchronization (semaphores)
- Priority handling
- Producer-consumer pattern

---

## Features

- Single runway simulation
- Emergency landing priority system
- Multiple plane types:
  - Landing planes
  - Takeoff planes
  - Emergency landing planes
- Real-time terminal UI using `ncurses`
- Automatic plane generation
- Manual plane creation via keyboard
- Thread-safe logging system (`logs.txt`)
- Airspace capacity control using semaphores

---

## Controls

While running the program:

| Key | Action |
|-----|--------|
| `a` | Add landing plane |
| `t` | Add takeoff plane |
| `e` | Add emergency landing plane |
| `q` | Quit simulation |

---

## Requirements

Install dependencies(for Ubuntu):

```bash
sudo apt update
sudo apt install gcc make libncurses5-dev libncursesw5-dev
