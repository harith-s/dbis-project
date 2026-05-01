~/postgres_custom/bin/pg_ctl -D ~/custom_data stop

~/postgres_custom/bin/pg_ctl -D ~/custom_data -o "-p 5433" -l ~/custom_postgres.log start

~/postgres_custom/bin/psql -p 5433 -d postgres