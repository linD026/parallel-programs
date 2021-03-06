# parallel-programs

---

## Deferred Mechanism

### Harzard Pointer (HP)
- **harzard pointer**:
    - The array-based hp in userspace.

### Read-Copy Update (RCU)

- **locked rcu**:
    - The user space rcu with global reference count.
- **classic rcu**:
    - The kernel space rcu with preemptible kernel.
	- Support sparse checking.
- **thrd-based rcu**:
    - The user space rcu with thread-local-storage reference count.
	- Implement concurrency linked-list.
	- Support sparse checking.

### Sequence Lock (Seqlock)
- **seqlock**:
    - The linux kernel style userspace seqlock.

---

## Concurrency Data Structure

### Skip List
- **ref**:
    - The cache friendly concurrency skiplist in the kernel space.
      It is from Liu Bo and Fusion-io.
- **src**:
    - The sequential program skiplist in userspace.
