// Test WeeIp tasks

#include "memory.h"
#include "task.h"

byte_t inc_border(byte_t b) {
    // Incease border color
    POKE(0xd020U,PEEK(0xd020U)+1);
    // Re-schedule task
    task_add(&inc_border, 45, 7,"border");
    return 1;
}

byte_t inc_background(byte_t b) {
    // Incease backgroud color
    POKE(0xd021U,PEEK(0xd021U)+1);
    // Re-schedule task
    task_add(&inc_background, 72, 10,"background");
    return 0;
}


void main() {
    // Init task system
    task_init();
    // Schedule tasks
    task_add(&inc_border, 45, 7,"border");
    task_add(&inc_background, 72, 10,"background");

    // Run scheduled tasks
    for(;;) {
        // Wait a frame
        while(PEEK(0xd012U)!=0xff) ;                
        // Call task periodic
        task_periodic();
    }

}