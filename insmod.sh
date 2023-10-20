make clean
make all -j
sudo rmmod trigger6 2>/dev/null
sudo insmod trigger6.ko
