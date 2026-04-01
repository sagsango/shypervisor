when we are in guest mode, cpu-traps like SEG_FAULT, Floating point exception
then VM_EXIT should not happen, the guest can just handle those by registering the irqs
