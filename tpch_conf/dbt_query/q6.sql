set enable_incremental to off;
set max_parallel_workers_per_gather to 0;
set work_mem to 1000000;

set enable_nestloop to off;
set enable_indexscan to off;
set enable_mergejoin to off;
set enable_dbtoaster to on;

set tpch_updates to 'lineitem';
set dbt_query to 'q6';

select
	q6_linenumber, sum(q6_extendedprice*q6_discount) as revenue
from
	q6
group by 
    q6_linenumber;

