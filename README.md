# FreeBSD veb(4) Network Pseudo Driver 
veb, vport FreeBSD driver

This Virtual Ethernet Bridge driver (veb) is a pseudo network device inspired by OpenBSD's veb(4) driver.

## How to use
### Compiling
In the repo's root directory:
1. Do: ```make``` to build the driver
2. Do: ```make vebctl``` to build the harness to use the driver

Everything compiled (if_veb.ko, vebctl, etc) is placed in the obj/ directory.

### Using the driver
* Once everything is compiled, load the driver: ```kldload obj/if_veb.ko```
* We can use ifconfig to create our veb and vport interfaces:
```sh
ifconfig veb create
ifconfig vport create
```
* To see all available options for interacting with a veb interface:
* Do: ```./obj/vebctl```
* Here are some examples of using vebctl to interact with our veb:
```sh
 ./obj/vebctl veb0 add vport0      # adding vport0 to veb0
 ./obj/vebctl veb0 del vport0      # deleting vport0 from veb0
 ./obj/vebctl veb0 show            # print veb0 member list (VEBGIFS)
 ./obj/vebctl veb0 rts             # print veb0 forwarding table (VEBGRTS)
 ./obj/vebctl veb0 flags vport0    # print vport0 member flags (VEBGIFFLGS)
 ./obj/vebctl veb0 setflags tap0 +sticky -discover      # adjust veb0 vport0 member flags
 ./obj/vebctl veb0 timeout         # print veb0 cache timeout (VEBGTO)
 ./obj/vebctl veb0 timeout 240     # set veb0 cache timeout (VEBSTO)
```
