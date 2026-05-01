# 📦 Using PostgreSQL as a Cache (pg_cache_router)

## Team

* Vaka Sri Varshitha (23b1030)
* Pranathi Sreeja (23b0936)
* Boyina Sanjana (23b0976)
* Kokkiligadda Manaswi (23b1046)

---

## Overview

This project implements a **transparent caching layer inside PostgreSQL** using a custom extension called **`pg_cache_router`**.

Instead of relying on external systems like Redis or Memcached, this approach:

* Uses a **second PostgreSQL instance as a cache**
* Preserves the **SQL interface**
* Avoids **application-level cache management**
* Maintains **consistency using logical replication**

---

## Motivation

Modern applications often face **read-heavy workloads**, where a small subset of data is accessed repeatedly.

Traditional caching introduces:

* Cache invalidation complexity
* Additional infrastructure (Redis/Memcached)
* Increased application logic
* Limited query expressiveness

### Our Approach

We integrate caching **inside PostgreSQL itself**, making it:

* Transparent to applications
* SQL-compatible
* Easier to maintain
* Consistent for writes

---

## System Architecture

```
Application
|
v SQL
+------------------- Cache server (port 5433) -------------------+
| planner_hook -> classify query |
| | |
| +-- PK-equality SELECT on cached table? |
| | | |
| | +-- yes -> normal plan + tag for executor |
| | | |
| | +-- no -> rewrite SELECT * FROM dblink(master, ...) |
| | |
| +-- INSERT/UPDATE/DELETE on routed table? |
| -> rewrite to dblink_exec |
| |
| ExecutorStart_hook (for tagged plans) |
| cache hit -> bump last_access, run normal plan locally |
| cache miss -> populate from master, then rewrite to dblink |
+---------------------------------------------------------------+
|
v
Master (5432)
|
v logical replication
Cache (5433) with row-filter trigger
```

---

## Features Implemented

### 1. Query Interception

* `planner_hook` → query classification
* `ExecutorStart` → runtime cache handling

---

### 2. Query Classification

* Primary-key SELECT → cacheable
* Complex SELECT → routed to master
* Writes → always routed to master

---

### 3. Read-Through Cache

* Cache HIT → serve locally
* Cache MISS →

  * fetch using `dblink`
  * insert into cache
  * return result

---

### 4. LRU Cache Management

Metadata table:

```
cache_lru_meta(table_name, pk_value, last_access)
```

Used for eviction policies.

---

### 5. Write Handling

* All writes executed on master via `dblink_exec`
* Cache invalidated after writes

---

## Project Structure

```
.
├── postgres/                # PostgreSQL 17.9 source code
├── pg_cache_router/         # Extension source code
├── configs/
│   ├── postgresql.conf      # Default configuration
│   ├── server1/             # Master configs
│   └── server2/             # Cache configs
├── diffs/
│   ├── server1_postgresql.diff
│   ├── server2_postgresql.diff
├── database setup files/    # SQL setup scripts
│   ├── cache setup
│   ├── main db setup
├── testing code/            # test scripts
│   ├── test.py
└── README.md
```

---

## ⚙️ Setup Instructions

### 1. Build PostgreSQL from Source

```bash
cd postgres
./configure
make -j$(nproc)
sudo make install
```

---

### 2. Initialize Servers

```bash
# Master
initdb -D ~/pg_master

# Cache
initdb -D ~/pg_cache
```

---

### 3. Configure Ports

* Master → `5432`
* Cache → `5433`

(See `configs/server1` and `configs/server2`)

---

### 4. Start Servers

```bash
pg_ctl -D ~/pg_master -l logfile start
pg_ctl -D ~/pg_cache -l logfile start
```

---

### 5. Install Extension

```bash
cd pg_cache_router
make
sudo make install
```

Add to `postgresql.conf`:

```
shared_preload_libraries = 'pg_cache_router'
```

Restart cache server after this.

---

### 6. Setup Database

All setup scripts are provided in:

```
database setup files/
```

---

### 🔹 Master Server (Port 5432)

```bash
psql -p 5432 -U postgres
```

Run in order:

```sql
\i 'database setup files/main db setup/master_setup.sql'
\i 'database setup files/main db setup/data files/largeRelationsInsertFile.sql'
\i 'database setup files/main db setup/data files/students.sql'
\i 'database setup files/main db setup/data files/instructors.sql'
\i 'database setup files/main db setup/data files/courses.sql'
```

---

### 🔹 Cache Server (Port 5433)

```bash
psql -p 5433 -U postgres
```

Run:

```sql
\i 'database setup files/cache setup/cache_setup.sql'
\i 'database setup files/cache setup/replication_filter_triggers.sql'
```

---

## Performance Observations

* Tested with 200 queries of simple SELECT
* 90% cache hit → ~0.5 sec
* 100% cache hit → ~0.3 sec

### Conclusion

* Significant latency reduction for repeated queries
* No correctness issues for complex queries

---

## Diffs Included

* `server1_postgresql.diff` → configuration differences from default for server1
* `server2_postgresql.diff` → configuration differences from default for server2

---

## Key Takeaways

* PostgreSQL can act as both **database + cache**
* Eliminates need for external caching systems
* Maintains **SQL compatibility + strong consistency**
* Transparent to applications

---
