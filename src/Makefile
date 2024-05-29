#make file 

obj-m := tcp_cubic_search.c
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd) 
default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules 

clean:
	$(RM) -f *.ko *.o *.symvers *.mod.c *.mod modules.order
