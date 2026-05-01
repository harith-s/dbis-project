drop table auto_test;

CREATE TABLE auto_test (user_id INT, username TEXT);
  INSERT INTO auto_test SELECT i, 'user_' || i FROM generate_series(1, 10000) i;
  SET log_min_messages = DEBUG1;

  SELECT * FROM auto_test WHERE user_id = 42; -> 6 times roughly

  // index created

  INSERT INTO auto_test SELECT i, 'user_' || i FROM generate_series(10000, 10050) i;

  // index dropped

  SELECT * FROM auto_test WHERE user_id = 42; -> 12 times

  SELECT * FROM auto_test WHERE username = 'user_42'; -> 6 times

  INSERT INTO auto_test SELECT i, 'user_' || i FROM generate_series(10050, 10110) i;

  // only user name is dropped



  ~/postgres_custom/bin/pg_ctl -D ~/custom_data -o "-p 5433" -l ~/custom_postgres.log stop
  ~/postgres_custom/bin/pg_ctl -D ~/custom_data -o "-p 5433" -l ~/custom_postgres.log start
  ~/pgsql-dev/bin/psql -p 5433 -d postgres


  on another terminal:
  watch tail ~/custom_postgres.log