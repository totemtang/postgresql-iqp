set max_parallel_workers_per_gather to 0;
set work_mem to 1000000;
set decision_method to dp;

set memory_budget to :v_budget;
set tpch_delta_mode to :v_mode; 
set bd_prob to :v_prob; 

set enable_incremental to on;
set tpch_updates to 'customer,orders,lineitem';
set iqp_query to 'q3';
set gen_mem_info to off; 

select
	i3_l_orderkey,
    sum(i3_l_extendedprice*(1-i3_l_discount)) as revenue,
	i3_o_orderdate, 
	i3_o_shippriority
from
    i3_c,
	i3_o,
	i3_l
where
	i3_c_custkey = i3_o_custkey
	and i3_l_orderkey = i3_o_orderkey
group by
	i3_l_orderkey,
	i3_o_orderdate,
	i3_o_shippriority
order by
	revenue desc,
	i3_o_orderdate;

