# README

Welcome to the Slow start Exit At Right CHoke point (SEARCH) TCP repository! This algorithm optimizes slow start exit precisely when needed. It analyzes delivered bytes, compares current and past data using RTT, and decides to exit slow start based on a predefined threshold.

## Dependency 

change the number `104` to `132` and `13` to `35`.  Then rebuild and reboot the kernel normally.

## Build

Follow these steps to integrate SEARCH TCP into your kernel:

* Add `tcp_cubic_search.c` file to `/net/ipv4/`

* Modify `net/ipv4/Kconfig` to include the SEARCH TCP configuration:
	  
	  config TCP_CONG_SEARCH
		tristate "SEARCH TCP"
		default n
		help
		   SEARCH TCP congestion control implements a search mechanism to dynamically adjust
  		   the congestion control state based on the observed network conditions. The algorithm
  		   divides time into bins and analyzes the sum total of delivered bytes within these bins
  		   to decide when to exit the slow start state and enter the congestion avoidance state.
  		  This decision is based on comparing the current delivered bytes to the delivered bytes
  		  one round-trip time ago and when the delivered bytes no longer increase, the capacity
  		  chokepoint has been detected.  Upon detection, SEARCH transitions the congestion control
  		  state from slow start to congestion avoidance.

* Add the `.o` file to `net/ipv4/Makefile`
  
  the line should look like: `obj-$(CONFIG_TCP_CONG_SEARCH) += tcp_cubic_search.o`
  
* Run the following commands:

    ```bash
    sudo make
    sudo make modules_install
    sudo make install
    ```

## Helpful Commands

Check available congestion control algs:

	sysctl net.ipv4.tcp_available_congestion_control

Check current congestion control alg:

	sysctl net.ipv4.tcp_congestion_control

Set current congestion control alg:

	sudo sysctl -w net.ipv4.tcp_congestion_control=cubic_search
    
	
Managing HyStart functionality:

	Disable hystart: 
 
 		sudo sh -c "echo '0' > /sys/module/tcp_cubic_search/parameters/hystart"
   
 	Enable hystart: 
  
  		sudo sh -c "echo '1' > /sys/module/tcp_cubic_search/parameters/hystart"


Managing SEARCH TCP functionality:

	Disable SEARCH: 
 
 		sudo sh -c "echo '0' > /sys/module/tcp_cubic_search/parameters/search"
   
 	Enable SEARCH with exit from slow start: 
  
  		sudo sh -c "echo '1' > /sys/module/tcp_cubic_search/parameters/search"
    
  	Enable SEARCH without exit from slow start (only logging information):
   
   		sudo sh -c "echo '2' > /sys/module/tcp_cubic_search/parameters/search"
   
Changing log level:  

	Enable Logging: 
 
 		sudo sh -c "echo '1' > /sys/module/tcp_cubic_search/parameters/search_logging"
    
  	Disable Logging:
   
   		sudo sh -c "echo '0' > /sys/module/tcp_cubic_search/parameters/search_logging"

Set congestion window (cwnd) at exit time:  

	Enable setting cwnd: 
 
 		sudo sh -c "echo '1' > /sys/module/tcp_cubic_search/parameters/set_cwnd"
    
  	Disable setting cwnd:
   
   		sudo sh -c "echo '0' > /sys/module/tcp_cubic_search/parameters/set_cwnd"

Apply interpolation in calculating previous window:  

	Enable interpolation: 
 
 		sudo sh -c "echo '1' > /sys/module/tcp_cubic_search/parameters/do_intpld"
    
  	Disable interpolation:
   
   		sudo sh -c "echo '0' > /sys/module/tcp_cubic_search/parameters/do_intpld"     
----------------
