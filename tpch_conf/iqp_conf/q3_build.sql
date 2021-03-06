DROP TABLE IF EXISTS I3_O CASCADE;
CREATE TABLE I3_O (
    I3_O_CUSTKEY         BIGINT,
    I3_O_ORDERKEY        BIGINT,
    I3_O_ORDERDATE       DATE,
    I3_O_SHIPPRIORITY    INTEGER
);

DROP TABLE IF EXISTS I3_C CASCADE;
CREATE TABLE I3_C (
    I3_C_CUSTKEY         BIGINT
);

DROP TABLE IF EXISTS I3_L CASCADE;
CREATE TABLE I3_L (
    I3_L_ORDERKEY        BIGINT,
    I3_L_EXTENDEDPRICE   DECIMAL,
    I3_L_DISCOUNT        DECIMAL
);
