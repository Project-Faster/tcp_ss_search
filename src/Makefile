#make file 

obj-m := tcp_cubic_search.o tcp_cubic.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd) 
default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules 

clean:
	${MAKE} -C $(KDIR) M=$(PWD) clean
