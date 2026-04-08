CREATE TABLE pg_autoindex_workload (
    queryid     bigint,
    query       text,
    calls       bigint,
    total_time  double precision,
    captured_at timestamptz DEFAULT now()
);

CREATE TABLE pg_autoindex_recommendations (
    id          serial primary key,
    tablename   text,
    columns     text[],
    index_type  text DEFAULT 'btree',
    est_benefit double precision,
    ddl         text,
    created_at  timestamptz DEFAULT now()
);