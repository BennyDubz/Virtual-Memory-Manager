# Virtual Memory State Machine
### Ben Williams, Summer 2024 - benjamin.r.williams.25@dartmouth.edu

## The Project

The goal of this project was to design and implement the memory management state machine that is the foundation of most commercial operating systems. The memory manager is able to provide the illusion that we have far more memory than is actually available in the physical hardware without any loss of data; this not only improves performance but allows the user to better take advantage of the hardware that they bought.


## The Simulation

I take advantage of Windows APIs that allow me to manage a chunk of physical memory directly. Then, the usermode threads simply pick a random address within a VirtualAlloc'd chunk of memory, and try to sequentially access a random number of pages. If they fault while accessing the address, then they will go into my pagefault routine to resolve the fault. Since the entire simulation is in usermode, there are some limitations and workarounds that I have to perform in order to do the same work as the operating system, but the concepts are all the same as a real-world operating system.

In order to run the simulation, you would need to be using a Windows computer and adjust their settings so that my program isn't flagged as a virus. To do this, go to Security Settings -> Local Policies -> User Rights Assignment and enable "“lock pages in memory" for your user. 

Then, to compile, just run "make" in the parent directory. See the makefile for other possible configurations.

## The Progression

### Single-threaded Implementation

Initially, I created a single-threaded version of the memory manager. This means that when a user thread had a page fault on an address that they would need to handle all of the workload themselves to resolve the fault. The faulter would have to find an available page, and might have to perform the trimming and writing to disk of other pages in order to succeed. While this was easier to implement, the user had to perform far too much work. Since memory management and pagefault-handling is just overhead to the user, we want to minimize the amount of work they had to perform.


### Basic Multi-threaded Implementation

For decades computers have used multiple processors and the operating systems have been adjusted entirely. The next step of this project was to make it possible for multiple faulting threads to run simultaneously, as well as have system threads in the background to reduce the workload on the user. 

To do this, I created a trimming thread and a modified-writer/disk-writer thread. Furthermore, I had to implement locks on the pagetables, the page lists, and the disk in order to synchronize all of the user and system threads. The goal was to create a working multi-threaded memory management state machine, but the lock contention was enormous and the scalability was unacceptable.


### Complex Multi-threading, Scalability


It is one thing to have a multi-threaded state machine, but another for it to actually scale: we wouldn't use 2 processors if it were twice as slow as 1. This is where the most difficult work is and where there are always improvements to be made. While I have made significant progress here, there is still work to be done. Nevertheless, here are some of the features I implemented to improve the scalability and speed of the program:

1. **Batching** - I batch trims, modified writes, and resolving pagefaults (speculating on sequential accesses). The overall goal was to reduce the amount of time spent in expensive operations (particularly MapUserPhysicalPages) by both the faulters and the system threads, and reduce the amount of pagefaults that happen overall

2. **Deferred work** - whether it is reading from the disk or zeroing out pages, the faulting thread can defer some of the cleanup work for later before batching it all together at once

3. **Fine-grained locks, reduced lock holdtimes, interlocked operations** - I use interlocked operations to have small spinlocks on individual pages and to remove the need for locks on the disk. These allow me to hold pagetable locks for less time, and to generally reduce lock contention on the larger locks. I also used reference counting inside of pages and inside of PTEs to reduce lock hold times.

4. **Shared locks** - Originally, I used critical sections in order to protect all of the linked list datastructures. However, with the use of pagelocks, I was able to move toward a shared-lock scheme so that multiple threads can edit the linked lists at the same time. This allows faulting threads to rescue pages from the modified/standby lists simultaneously while rarely colliding with eachother or the other system threads

5. **Preserved Pagefile Space Through Distinguishing Read / Read-Write Permissions and Accesses** - When an address is read from rather than written to, if we have a copy of the memory stored on the disk we can keep it there rather than immediately invalidating it if we failed to distinguish between reads and writes. This allows us to trim pages directly to standby and reduce the amount of time that the modified-writer thread needs to work

6. **Additional System Worker Threads** - I implemented threads that would keep the free and zero lists replenished with pages, reducing contention on the standby list. Moreover, I implemented a thread that can zero-out many pages at once. This reduces the overall workload on the faulters if there is a sudden demand for zeroed pages.


There are many more smaller optimizations and many tricks I have used along the way in order to improve performance. It is always a work in progress - my scalability is not perfect yet but has gotten better over time. My decisions here are informed by many performance traces where I can hunt down lock contention problems and other scalability issues.


## Works in Progress & Limitations

1. I would like to further reduce the lock contention on the PTE lock by not having to hold it while unmapping PTEs in the trimming thread, I have not yet implemented the reference counting scheme that would be needed. **Completed as of October 4th, 2024: I added an extra field to the valid PTE format to handle when it is being changed. This allowed me to reduce lock holdtimes for unaccessed-pte faulters and for the trimming, which has now completely eliminated my remaining PTE lock contention**

2. One of the largest limitations of this project is the lack of an aging implementation. The difficulty here stems from the fact that we are running a simulation and the CPU isn't changing our PTE structures to account for access bits and age. In the simulation, we would have to manually acquire the PTE lock **even when there wasn't a pagefault**, and set either/both the age and access bits if the PTE was still valid. This would create an enormous amount of overhead that we would not have to deal with in the real world, as it opens the possibility for far more PTE lock contention. This means that the trimmer is not always trimming the best candidates. **Update as of October 17th: I was able to implement an access bit on valid PTEs using InterlockedCompareExchange operations. This allows us to have at least one simple aging heuristic for trimming. It is still not as realistic as a real world system, but it does help to have informed trimming.**





