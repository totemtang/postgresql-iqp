DROP TABLE IF EXISTS Q3_REFRESH CASCADE;
CREATE TABLE Q3_REFRESH (
    Q3_REFRESH_ORDERKEY        BIGINT,
    Q3_REFRESH_REVENUE	       DECIMAL,
    Q3_REFRESH_ORDERDATE       DATE,
    Q3_REFRESH_SHIPPRIORITY    INTEGER
);
