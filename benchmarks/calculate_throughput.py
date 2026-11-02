operations = int(input("Completed operations: "))
elapsed = float(input("Elapsed time in seconds: "))

if operations <= 0 or elapsed <= 0:
    print("Both values must be positive.")
else:
    throughput = operations / elapsed

    print(f"Throughput: {throughput:,.2f} operations/second")
    print(f"Throughput: {throughput / 1_000_000:.4f} Mops/s")
