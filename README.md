# LogFS Key‑Value Store

A simple key‑value database in **C** built atop a custom log‑structured file system (LogFS).  
It provides persistent storage with an interactive shell for inserting, removing, and querying keys.

---

## 🚀 Features

- **Persistent Log‑Structured Storage**  
  Appends all writes to a file-backed log for fast, sequential I/O.

- **Key‑Value API**  
  - `put <key> <value>` — insert or update an entry  
  - `get <key>` — retrieve a value  
  - `del <key>` — delete an entry  
  - `list` — show all stored keys  

- **Interactive Shell**  
  - Command history and simple line editing via `term.c`  
  - `help` menu for available commands  

- **Indexing & Lookup**  
  - In‑memory index (`index.c/h`) for fast key lookups  
  - Efficient data layout with metadata headers (`logfs.h`, `kvraw.c`)

---

## 🛠️ Tech Stack

- **Language:** C  
- **Build:** GNU Make & GCC (`-ansi -pedantic -Wall -O3`)  
- **Core Modules:**  
  - `logfs.c / logfs.h` — LogFS core routines  
  - `kvraw.c / kvraw.h` — Raw key‑value operations on LogFS  
  - `kvdb.c / kvdb.h` — Higher‑level KVDB API  
  - `index.c / index.h` — In‑memory index management  
  - `system.c / system.h` — Utility functions (timing, file ops)  
  - `term.c / term.h` — Terminal I/O helpers  
  - `main.c` — CLI front‑end  
- **Extras:** Auto‑generated dependency files (`*.d`)

---

## ⚙️ Build & Run

```bash
# Compile the executable
make

# Launch the REPL shell
./cs238 <backing-file>

# Example: start fresh
./cs238 data.log --truncate

# In the shell:
> put foo bar
> get foo
bar
> list
foo
> del foo
> exit
