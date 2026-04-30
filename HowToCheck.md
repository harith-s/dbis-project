
# Automated Index Generation - Setup & Demo

---

## 1. Build PostgreSQL (Custom Installation)

```bash
# 1. Configure the build to go to a local folder in your home directory
./configure --prefix=$HOME/postgres_custom --enable-debug

# 2. Compile the code (this will take a few minutes)
make -j$(sysctl -n hw.ncpu)

# 3. Install to the $HOME/postgres_custom folder
make install
```

---

## 2. Initialize Database

```bash
# Create a data directory
mkdir ~/custom_data

# Use your NEWLY compiled initdb to set up the database
~/postgres_custom/bin/initdb -D ~/custom_data
```

---

## 3. Start PostgreSQL Server

```bash
# Start the server on port 5433
~/postgres_custom/bin/pg_ctl -D ~/custom_data -o "-p 5433" -l ~/custom_postgres.log start
```

---

## 4. Connect to PostgreSQL

```bash
~/postgres_custom/bin/psql -p 5433 -d postgres
```

---

## 5. Create Test Table

```sql
CREATE TABLE auto_test (
    user_id INT,
    username TEXT
);
```

---

## 6. Insert Sample Data

```sql
INSERT INTO auto_test (user_id, username)
SELECT g, 'user_' || g
FROM generate_series(1, 1000) g;
```

---

## 7. Verify Table

```sql
\d auto_test
```

---

## 8. Trigger Sequential Scan (Auto Indexing)

```sql
SELECT * FROM auto_test WHERE user_id = 500 and username = 'user_500';
```

---

## 9. Check Index Creation

```sql
\d auto_test
```

---

# Recompiling After Code Changes

If you modify PostgreSQL source code, follow these steps:

```bash
# Compile only the changed files and link the new postgres binary
make -j$(sysctl -n hw.ncpu)

# Move the newly compiled binaries into your installation folder
make install
```

---

## Restart the Server

```bash
# Stop the running custom server
~/postgres_custom/bin/pg_ctl -D ~/custom_data stop

# Start it back up (using the same port and log file as before)
~/postgres_custom/bin/pg_ctl -D ~/custom_data -o "-p 5433" -l ~/custom_postgres.log start
```

---

## Connect to Updated Server

```bash
~/postgres_custom/bin/psql -p 5433 -d postgres
```

---

## Summary

* Build → Initialize → Start → Test → Auto Indexing
* Modify code → Recompile → Restart → Continue testing

---