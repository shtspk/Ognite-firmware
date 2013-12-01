/*
 * Candle0005.c
 *
 * Created: 4/9/2013 12:01:43 AM
 *  Author: josh
 */ 

/*
	09/21/13 - Reversed matrix lookup table to cancel out mirrored physical boards. Boards got mirrored accidentally and easier to change software than reprint.
	09/22/13 - Reversed the rowDirectionBits to fix the mess caused by the previous change. Argh.  
	09/26/13 - First GitHub commit
	10/16/13 - Added test mode on that will show a test pattern on power-up.
	10/31/13 - Starting to whittle. Program Memory Usage 	:	3254 bytes   
	10/31/13 - Changed everything except ISR to static inline. Program Memory Usage 	:	3240 bytes
	11/07/13 - video now plays a frame at a time aysynchonously and things generally cleaned up
	11/21/13 - Switched over to running off the WatchDog and sleeping the rest of the time

*/


// TODO: Check if changing SUT to reduce the startup delay to only 4 cycles (without the default 64ms) would save power. 
//       Might neeed to turn on BOD and then turn it off when we sleep using BODCR

// TODO: Once everything is done, could set Watchdog fuse and then would not have to have code to turn it on at power up. Might also preserve the prescaler between resets, saving more time and code
// TODO: Once everything is done, could clear the the CLK/8 bit and have chip startup running fast and not have to do in code on every wake. 

// TODO: Maybe try to get everything running with fuze-set 4mhz so that we can run battery down to 2.7 volts 


#define F_CPU 8000000UL  // We will be at 8 MHz once we get all booted up and get rid of the prescaller

#include <avr/io.h>

#include <avr/pgmspace.h>

#include <avr/interrupt.h>

#include <avr/sleep.h>

#include <avr/wdt.h>				// Watchdog Functions

#include <string.h>					// memset()


//#define PRODUCTION				// Production burn?

#ifdef PRODUCTION

	FUSES = {
	
		.low = (FUSE_SUT1 & FUSE_SUT0 & FUSE_CKSEL3 & FUSE_CKSEL1 & FUSE_CKSEL0),			// Startup with clock/8, no startup delay, 8Mhz internal RC
		.high =  HFUSE_DEFAULT,
		.extended = EFUSE_DEFAULT
	
	};
	
#else
	
	FUSES = {
		
		.low = (FUSE_CKDIV8 & FUSE_SUT1 & FUSE_SUT0 & FUSE_CKSEL3 & FUSE_CKSEL1 & FUSE_CKSEL0),			// Startup with clock/8, no startup delay, 8Mhz internal RC
		.high =  HFUSE_DEFAULT,
		.extended = EFUSE_DEFAULT
		
	};
		
#endif


#include "candle.h"

#include "videoBitstream.h"


// Convert 5 bit brightness into 8 bit LED duty cycle

// Note that we could keep this in program memory but we have plenty of static RAM right now 
// and this way makes slightly smaller code


#define DUTY_CYCLE_SIZE (1<<BRIGHTNESSBITS)

#define FULL_ON_DUTYCYCLE 255	// how much is a full on LED?

const byte brightness2Dutycycle[DUTY_CYCLE_SIZE] = {
	0,     1 ,     2,     3,     4,     5,     7,     9,    12,
	15,    18,    22,    27,    32,    38,    44,    51,    58,
	67,    76,    86,    96,   108,   120,   134,   148,   163,
	180,   197,   216,   235,   255,
};

#define getDutyCycle(b) (brightness2Dutycycle[b])

#define FDA_X_MAX ( (byte) 5 )
#define FDA_Y_MAX ( (byte) 8 )

#define FDA_SIZE ( (byte) (FDA_X_MAX*FDA_Y_MAX) )

// This is the display array!
// Array of duty cycles levels. Brightness (0=off,  255=brightest)
// Anything you put in here will show up on the screen

// Now fda is linear - with x then y for easy scanning in x direction

// Graph paper style - x goes left to right, y goes bottom to top 

byte fda[FDA_SIZE];

// Set up the pins - call on startup


#define REFRESH_RATE ( (byte) 62 )			// Display Refresh rate in Hz (picked to match the fastest we can get WDT wakeups)

#define TIMECHECK 1				// Twittle bits so we can watch timing on an osciliscope
								// PA0 (pin 5) goes high while we are in the screen refreshing/PWM interrupt routine
								// PA1 (pin 4) goes high while we are decoding the next frame to be displayed


#define ROWS FDA_Y_MAX
#define COLS FDA_X_MAX

		
#define NOP __asm__("nop\n\t")

byte diagPos=0;		// current screen pixel when scanning in diagnostic modes 0=starting to turn on, FDA_SIZE=starting to turn off, FDA_SIZE*2=done with diagnostics

// Decode next frame into the FDA

static inline void nextFrame(void) {
	
	  #ifdef TIMECHECK
		  PORTA |= _BV(1);
	  #endif
	  
	  // TODO: this diagnostic screen generator costs 42 bytes. Can we make it smaller or just get rid of it?
	  
	  // TODO: For production version, probably take out brightness test but first make sure 5 bits is really visible. 
		
	  if (diagPos<(FDA_SIZE*4)) {		// We are currently generating the startup diagnostics screens
		  
		  if (diagPos<FDA_SIZE) {						// Fill screen in with pixels
			  
			  fda[diagPos] = FULL_ON_DUTYCYCLE;
			  
		  } else if (diagPos<FDA_SIZE*2) {				// Empty out
			  
			  fda[(diagPos)-FDA_SIZE] = 0;
		  
		  } else /* if (diagPos>=FDA_SIZE*2) && (diagPos<FDA_SIZE*4) */ {										// Brightness test pattern
			  
				byte step=( diagPos-(FDA_SIZE*2) );
			
				byte fdaptr = 0;
			
				for(byte y=0; y<FDA_Y_MAX;y++) {
				
					byte b = getDutyCycle( step & (_BV(BRIGHTNESSBITS)-1) );				// normalize step variable to always cycle within brightness range
				
					for(byte x=0;x<FDA_X_MAX;x++) {
						fda[fdaptr++] = b;
					}
				
					step++;
				}
			  			 
			
			/*  
			for(byte x=0;x<FDA_SIZE;x++ ) {
				fda[x] =getDutyCycle( (x) & (_BV(BRIGHTNESSBITS)-1) );
			}
			*/

			
		  }
			  
		  diagPos++;
		  
	   } else {  // normal video playback....
		  
		  // Time to display the next frame in the animation...
		  // copy the next frame from program memory (candel_bitstream[]) to the RAM frame buffer (fda[])
		  
		  		  
		  static byte const *candleBitstremPtr;     // next byte to read from the bitstream in program memory

		  static byte workingByte;			  // current working byte
		  
		  static byte workingBitsLeft;      // how many bits left in the current working byte? 0 triggers loading next byte
		  
		  static framecounttype frameCount = FRAMECOUNT;		// what frame are we on?

		  
		  if ( frameCount==FRAMECOUNT ) {							// Is this the last frame?
			  
			  memset( fda , 0x00 , FDA_SIZE );			// zero out the display buffer, becuase that is how the encoder currently works
			  candleBitstremPtr=videobitstream;		// next byte to read from the bitstream in program memory
			  workingBitsLeft=0;							// how many bits left in the current working byte? 0 triggers loading next byte
			  frameCount= 0;
			  
		  }
		  
		  frameCount++;
		  
		  byte fdaIndex = FDA_SIZE;		// Which byte of the FDA are we filling in? Start at end because compare to zero slightly more efficient and and that is how data is encoded
		  
		  byte brightnessBitsLeft=0;	// Currently building a brightness value? How many bits left to read in?
		  
		  byte workingBrightness;		// currently building brightness value
		  
		  do {			// step though each pixel in the fda
			  
			  
			  if (workingBitsLeft==0) {										// normalize to next byte if we are out of bits
				  
				  workingByte=pgm_read_byte_near(candleBitstremPtr++);
				  workingBitsLeft=8;
				  
			  }
			  
			  byte workingBit = (workingByte & 0x01);
			  
			  if (brightnessBitsLeft>0) { //are we currently reading brightness? Consume as many bits as we can (if too few) or as we need (if too manY) from working byte into brightness
				  				  
				  workingBrightness <<=1;
				  workingBrightness |= workingBit;
				  
				  brightnessBitsLeft--;
				  
				  if (brightnessBitsLeft==0) {		// We've gotten enough bits to assign the next pixel!

					  fda[--fdaIndex] = getDutyCycle(workingBrightness);
					  
				  }
					  				  
			  } else { // We are not currently reading in a pending brightness value 
					  				  
				  if ( workingBit ==  0x00 ) {		// 0 bit indicates that this pixel has not changed
					  
					  --fdaIndex;								// So skip it
					  
				  } else {							// Start reading in the following bits as a brightness value
					  
					  brightnessBitsLeft = BRIGHTNESSBITS;					// Now we will read in the brightness value on next loops though
					  workingBrightness = 0;
					  
				  }
				  
			  }
			  
			  workingByte >>=1;				
			  workingBitsLeft--;
			  
		  } while ( fdaIndex > 0 );
		  		  
		  #ifdef TIMECHECK
			 PORTA  &= ~_BV(1);
		  #endif
		  
	  }
	  
}


#define ALL_PORTD_ROWS_ZERO 1		// Just a shortcut hard-coded that all PORTD row bits are zero 

static byte const rowDirectionBits = 0b01010101;      // 0=row goes low, 1=Row goes high

static byte const portBRowBits[ROWS]  = {_BV(0),_BV(0),_BV(2),_BV(2),_BV(4),_BV(4),_BV(6),_BV(6) };
static byte const portDRowBits[ROWS]  = {     0,     0,     0,     0,     0,     0,     0,     0 };
	
// Note that col is always opposite of row so we don't need colDirectionBits

static byte const portBColBits[COLS] = {_BV(7),_BV(5),_BV(3), _BV(1),     0};
static byte const portDColBits[COLS] = {     0,     0,      0,     0,_BV(6)};
	

#define REFRESH_PER_FRAME ( REFRESH_RATE / FRAME_RATE )		// How many refreshes before we trigger the next frame to be drawn?

byte refreshCount = REFRESH_PER_FRAME+1;


// Do a single full screen refresh     
// call nextframe() to decode next frame into buffer afterwards if it is time
// This version will work with any combination of row/col bits



static inline void refreshScreenClean(void)
{
	
	#ifdef TIMECHECK
		DDRA = _BV(0)|_BV(1);		// Set PORTA0 for output. Use OR because it compiles to single SBI instruction
		PORTA |=_BV(0);				// twiddle A0 bit for oscilloscope timing
	#endif
	
	byte *fdaptr = fda;		 // Where are we in scanning through the FDA?
	
	byte rowDirectionBitsRotating = rowDirectionBits;	// Working space for rowDirections bits that we shift down once for each row
	// Bit 0 will be bit for the current row.
	// TODO: in ASM, would could shift though the carry flag and jmp based on that and save a bit test
	
	for( byte y = 0 ; y < FDA_Y_MAX ; y++ ) {

		byte portBRowBitsCache = portBRowBits[y]; 
		byte portDRowBitsCache = portBRowBits[y]; 
		
		for( byte x = 0 ; x < FDA_X_MAX ; x++) {
			
			// get the brightness of the current LED

			register byte b = *( fdaptr++ );		// Want this in a register because later we will loop on it and want the loop entrance to be quick
			
			// If the LED is off, then don't need to do anything since all LEDs are already off all the time except for a split second inside this routine....

			if (b>0) {
				
				byte portBColBitsCache = portBColBits[x]; 
				byte portDColBitsCache = portDColBits[x]; 
				
				// Assume DDRB = DDRD = 0 coming into the INt since that is the Way we should have left them when we exited last...

				byte ddrbt;
				byte ddrdt;


				if ( rowDirectionBitsRotating & _BV(0) ) {    // lowest bit of the rotating bits is for this row. If bit=1 then row pin is high and col pins are low....
					
					PORTB = portBRowBitsCache;
					
					PORTD = portDRowBitsCache;

					// Only need to set the correct bits in PORTB and PORTD to drive the row high (col bit will get set to 0)

					ddrbt  = portBRowBitsCache | portBColBitsCache ;       // enable output for the Row pins to drive high, also enable output for col pins which are zero so will go low
					
					ddrdt  = portDRowBitsCache | portDColBitsCache;

				} else {      // row goes low, cols go high....

					
					PORTB = portBColBitsCache;
					ddrbt  = portBColBitsCache | portBRowBitsCache;               // enable output for the col pins to drive high, also enable output for row pins which are zero so will go low
					
					PORTD = portDColBitsCache;					
					ddrdt  = portDColBitsCache | portDRowBitsCache;
					
				}
				
				// Now comes the business of Actually turning on the LED.
				
				// This is the tightest loop possible - I can't figure out how to do it in C
				
				// while (b--) asm volatile ("");		//does not work because it generates a strange RJMP +0 loop preamble

				// _delay_loop_1( b );					// DOes not work becuase it pathalogically loads b into a register


				// TODO: Do actual visual test to make sure that actual brightness is linear and smooth with this algorthim
				//       Might not be because this does not take into account delay of loop branching. It would take at least 5 lines of
				//		 code to get this timing exactly right by special casing out b=1 and b=2, and then dividing higher numbers to account for the branch cost
				//	     would be better to have this already accounted into the precomputed brightness->dutycycle map.
				 
				 
				if (b==1) {		// Special case low values because loop overhead will mes sup on time...
								
					asm volatile (
								
						"OUT %0,%1 \n\t"				// DO DDRD first because in current config it will never actually have both pins so LED can't turn on (not turn of DDRB)
						"OUT %2,%3 \n\t"				// Ok, LED is on now!
						// No delay at all, will be off on next instruction
						"OUT %2,__zero_reg__ \n\t"		// Do DDRB first since this will definitely turn off the LED
						"OUT %0,__zero_reg__ \n\t"
				
						: :  "I" (_SFR_IO_ADDR(DDRD)) , "r" (ddrdt) , "I" (_SFR_IO_ADDR(DDRB)) , "r" (ddrbt) 
				
					);
				
				} else if (b==2 && 0 ) {
				
					asm volatile (
					
						"OUT %0,%1 \n\t"			// DO DDRD first because in current config it will never actually have both pins so LED can't turn on (not turn of DDRB)
						"OUT %2,%3 \n\t"			// Ok, LED is on now!
						"NOP\n\t"
						"OUT %2,__zero_reg__ \n\t"			// Do DDRB first since this will definitely turn off the LED
						"OUT %0,__zero_reg__ \n\t"
					
						: :  "I" (_SFR_IO_ADDR(DDRD)) , "r" (ddrdt) , "I" (_SFR_IO_ADDR(DDRB)) , "r" (ddrbt)
					
					);
				
				} else if (b==3) {
			
					asm volatile (
			
						"OUT %0,%1 \n\t"			// DO DDRD first because in current config it will never actually have both pins so LED can't turn on (not turn of DDRB)
						"OUT %2,%3 \n\t"			// Ok, LED is on now!
						"NOP\n\t"
						"NOP\n\t"
						"OUT %2,__zero_reg__ \n\t"			// Do DDRB first since this will definitely turn off the LED
						"OUT %0,__zero_reg__ \n\t"
			
						: :  "I" (_SFR_IO_ADDR(DDRD)) , "r" (ddrdt) , "I" (_SFR_IO_ADDR(DDRB)) , "r" (ddrbt) 
			
					);
					
				} else if (b>=4) {
					
				
					b /= 2;		// Account for the fact that loop has overhead
			
					asm volatile (
			
						"OUT %0,%1 \n\t"			// DO DDRD first because in current config it will never actually have both pins so LED can't turn on (not turn of DDRB)
						"OUT %2,%3 \n\t"			// Ok, LED is on now!
						"L_%=:dec %4 \n\t"
						"BRNE L_%= \n\t"
						"OUT %2,__zero_reg__ \n\t"			// Do DDRB first since this will definitely turn off the LED
						"OUT %0,__zero_reg__ \n\t"
			
						: :  "I" (_SFR_IO_ADDR(DDRD)) , "r" (ddrdt) , "I" (_SFR_IO_ADDR(DDRB)) , "r" (ddrbt) , "r" (b)
			
					);
					
				}
		
			} // b==0
		}
		
		rowDirectionBitsRotating >>= 1;		// Shift bits down so bit 0 has the value for the next row
	}
	
	#ifdef TIMECHECK
		PORTA &= ~_BV(0);
	#endif
	
	
	refreshCount--;
	
	if (refreshCount == 0 ) {			// step to next frame in the animation sequence?
		
		refreshCount=REFRESH_PER_FRAME+1;
		
		// Update the display buffer with the next frame of animation
				
		nextFrame();
		
	}
	
	
}


void init0 (void) __attribute__ ((naked)) __attribute__ ((section (".init0")));

// This code will be run immedeately on reset, before any initilization or main()

void init0(void) {

	asm( "in	__tmp_reg__	, %[mcusr] "	: : [mcusr] "I" (_SFR_IO_ADDR(MCUSR)) ); 	// Get the value of the MCUSR register into the temp register
	asm( "sbrc	__tmp_reg__	,%[wdf] "		: : [wdf] "I" (WDRF) );						// Test the WatchDog Reset Flag and skip the next instruction if the bit is clear
	asm( "rjmp warmstart" 					: :   );									// If we get to here, the the WDF bit was set so we are waking up warm, so jump to warm code
	
	// On power up, This code will fall though to the normal .init seconds and set up all the variables and get ready for main() to start
}

// TODO: Make our own linker script and move warmstart here so we save one cycle on the RJMP, and instead take that hit only once with a jump to main() on powerup


// Put any code you want to run once on power-up here....
// Any global variables set in this routine will persist to the WatchDog routine
// Note that I/O registers are initialized on every WatchDog reset the need to be updated inside userWakeRountine()

// This is "static inline" so The code will just be inserted directly into the main() code avoiding overhead of a call/ret

static inline void userStartup(void) {

	// Your start-up code here....
	
}

// Main() only gets run once, when we first power up

int main(void)
{
			
	wdt_enable(WDTO_15MS);							// Could do this slightly more efficiently in ASM, but we only do it one time- when we first power up
	
	// The delay set here is actually just how long until the first watchdog reset so we will set it to the lowest value to get into cycyle as soon as possible
	// Once in the cycle,  warmstart will set it to the desired running value each time.
	
	// Note that we could save a tiny ammount of time if we RJMPed right into the warmstart() here, but that
	// could introduce a little jitter since the timing would be different on the first pass. Better to
	// always enter warmstart from exactly the same state for consistent timing.
	
	
	MCUCR = _BV( SE ) |	_BV(SM1 ) | _BV(SM0);		// Sleep enable (makes sleep instruction work), and sets sleep mode to "Power Down" (the deepest sleep)
	
	asm("sleep");
	
	// we should never get here
	
}


// This is "static inline" so The code will just be inserted directly into the warmstart code avoiding overhead of a call/ret
// Important that this function always finishes before WDT expires or it will get cut short

static inline void userWakeRoutine(void) {
		
	refreshScreenClean();
	
}

void  __attribute__ ((naked)) warmstart(void) {
	
	// Set the timeout to the desired value. Do this first because by default right now it will be at the inital value of 16ms
	// which might not be long enough for us to do what we need to do before the WatchDog times out and does a reset.
	
	// Note that we do not have to set WDTCR because the default timeout is initialized to 16ms after a reset (which is now). This is ~60Hrz display refresh rate.
	
	
	#ifndef PRODUCTION						// In produciton build, the FUSE will already have us start at full speed
	
		CLKPR = _BV(CLKPCE);				// Enable changes to the clock prescaler
		CLKPR = 0;							// Set prescaler to 1, we will run full speed						
											// TODO: Check if running full speed uses more power than doing same work longer at half speed

	#endif
	
	// Now do whatever the user wants...
	
	userWakeRoutine();
	
	// Now it is time to get ready for bed. We should have gotten here because there was just a WDT reset, so...
	// By default after any reset, the watchdog timeout will be 16ms since the WDP bits in WDTCSR are set to zero on reset. We'd need to set the WDTCSR if we want a different timeout
	// After a WDT reset, the WatchDog should also still be on by default because the WDRF will be set after a WDT reset, and "WDE is overridden by WDRF in MCUSR. See �MCUSR � MCU Status Register� on page 45for description of WDRF. This means thatWDE is always set when WDRF is set."
		
	
	MCUCR = _BV( SE ) |	_BV(SM1 ) | _BV(SM0);		// Sleep enable (makes sleep instruction work), and sets sleep mode to "Power Down" (the deepest sleep)
		
	asm("sleep");
	
	// we should never get here
	
}