import matplotlib.pyplot as plt

delay_us      = [0, 1, 2, 5, 10, 20, 30, 40, 50, 100, 150, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000]
num_aborts    = [5545, 4720, 4681, 4907, 3945, 3596, 3687, 3807, 3847, 3816, 4152, 3562, 2963, 1834, 2010, 1088, 648, 365, 155, 81, 46, 16, 9]
num_ioctl     = [265397, 941541, 794619, 777816, 986329, 911024, 1005291, 1119316, 965428, 978040, 999320, 1018376, 1273230, 940867, 1198067, 1180471, 1236464, 1294406, 1230991, 1109617, 1426491, 1434242, 1162498]

abort_rate = [a / c for a, c in zip(num_aborts, num_ioctl)]

plt.figure()
plt.plot(delay_us, abort_rate, marker='o')
plt.xlabel("Write→read delay (µs)")
plt.ylabel("Abort rate (EAGAIN / ioctl calls)")
plt.title("Verifier delay vs. remapper abort rate")
plt.tight_layout()
plt.savefig("abort-ratio.png")
plt.show()
