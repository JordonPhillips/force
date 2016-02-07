/* Copyright (C) 2011-2016 by Jacob Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// ----- Includes -----

// Compiler Includes
#include <Lib/delay.h>
#include <Lib/MainLib.h>

// Project Includes
#include <output_com.h>

#include <cli.h>
#include <led.h>
#include <print.h>



// ----- Defines -----

// Wider range of measurement values (up to 1kg absolute max, 600g is probably the safest...)
#define VREF3_3
// More accurate, will top out around 600g
//#define VREF1_2



// ----- Macros -----



// ----- Enums -----

typedef enum ForceCaptureMode {
	ForceCaptureMode_OnRequest          = 0x0,

	ForceCaptureMode_FreeRun_Serial     = 0x1,
	ForceCaptureMode_FreeRun_Speed      = 0x2,

	ForceCaptureMode_FreeRun_Full       = 0xFF,
} ForceCaptureMode;

typedef enum Direction {
	Direction_None = 0,
	Direction_Up   = 1,
	Direction_Down = 2,
} Direction;



// ----- Structs -----

// Struct of all free-running datapoints
typedef struct ForceCurveDataPoint {
	uint32_t time;
	uint32_t distance_raw;     // Raw value, starting position is random on startup
	uint32_t distance;         // Includes adjusted offset to prevent overflow
	uint16_t speed;            // TODO
	uint16_t force_adc;        // Current ADC force value
	uint16_t force_adc_max;    // Max ADC force in the last iteration (resets between tests)
	uint8_t  continuity;       // If hooked up, will indicate when switch is pressed
	uint8_t  direction;        // Current moving direction (also indicates non-moving condition)
	char     force_serial[10]; // Serial ADC data (calibrated), causes issues with ADC so not run very often
} __attribute__((packed)) ForceCurveDataPoint;

// Test State
typedef struct ForceCurveTestState {
	uint32_t distance_bottom; // Determined top start of test (TODO)
	uint32_t distance_top;    // Determined bottom of press (required to run the motor faster with the force sensor)
	uint16_t test_total;      // Total tests to run
	uint16_t test_cur;        // Current test
	uint16_t adc_origin;      // Initial force from the ADC (in case there is an offset to apply)
	uint8_t  running;         // If a test if current running
} ForceCurveTestState;



// ----- Function Declarations -----

void cliFunc_buzz        ( char* args );
void cliFunc_down        ( char* args );
void cliFunc_free        ( char* args );
void cliFunc_stat        ( char* args );
void cliFunc_stop        ( char* args );
void cliFunc_test        ( char* args );
void cliFunc_up          ( char* args );
void cliFunc_zero        ( char* args );

void buzzer_set( uint16_t val );

void motor_stop();
void motor_up_start();
void motor_down_start();

uint32_t timer_timestamp();



// ----- Variables -----

// Force Gauge command dictionary
CLIDict_Entry( buzz,  "Set buzzer level, default to off" );
CLIDict_Entry( down,  "Enable motor down signal" );
CLIDict_Entry( free,  "Toggle free-running modes: f - full on; o - full off; s - serial" NL "\t\tp - speed" );
CLIDict_Entry( stat,  "Current measurements, will query devices if not in free-running mode" );
CLIDict_Entry( stop,  "Stop motor movement" );
CLIDict_Entry( test,  "Run switch test. First test determines how far the press goes (should be slow)" NL "\t\tSubsequent tests only go that far. Argument is # of tests. Default 1" );
CLIDict_Entry( up,    "Enable motor up signal" );
CLIDict_Entry( zero,  "Zero gauges" );

CLIDict_Def( forceGaugeCLIDict, "Force Curve Gauge Commands" ) = {
	CLIDict_Item( buzz ),
	CLIDict_Item( down ),
	CLIDict_Item( free ),
	CLIDict_Item( stat ),
	CLIDict_Item( stop ),
	CLIDict_Item( test ),
	CLIDict_Item( up ),
	CLIDict_Item( zero ),
	{ 0, 0, 0 } // Null entry for dictionary end
};


// Default Capture mode
ForceCaptureMode Main_ForceCaptureMode = ForceCaptureMode_OnRequest;

// Free-running datastore
volatile ForceCurveDataPoint Main_FreeRunData;

// Test Data
volatile ForceCurveTestState Main_TestState;



// ------ Distance Measurement ------

// PWM Input Interrupt
volatile uint8_t  distance_pulses_on = 0;     // FTM PWM Pulse Clock for Distance Gauge
volatile uint8_t  distance_bit_pos = 0;       // Bit-bang bit position of Distance Gauge
volatile uint32_t distance_read_data = 0;     // Bit history of Distance Gauge read
volatile uint8_t  distance_zero_next = 1;     // Signal to re-calibrate/zero the offset
volatile  int32_t distance_offset = 0;        // Offset value to use
volatile uint8_t  distance_offset_enable = 0; // Enable/disable offset depending on conditions
void pit0_isr()
{
	// If pulses are on, turn off
	if ( distance_pulses_on )
	{
		// Disable FTM PWM
		FTM0_C7SC = 0x00;

//#define SPI_DIST
#ifdef SPI_DIST
		// Read spi
		// TODO Cleanup
		print(NL);
		printHex( SPI0_SR & SPI_SR_RXCTR );
		print(" ");

		// Expecting to read 3 16 bit values from the FIFO
		// Only the first 7 bits have useful data
		if ( ( ( SPI0_SR & SPI_SR_RXCTR ) >> 4 ) > 0 )
		{
			uint32_t data = (SPI0_POPR << 16) | (SPI0_POPR << 8) | (SPI0_POPR << 1);
			asm( "rbit %1,%0" : "=r" ( data ) : "r" ( data ) ); // Cortex M4 Instruction MSB to LSB
			printHex32( data >> 8 );
		}

		// Flush Rx FIFO
		SPI0_MCR |= SPI_MCR_CLR_RXF;
#else
		// Reset bit position
		distance_bit_pos = 0;
#endif
	}
	// Otherwise turn them on
	else
	{
#ifdef SPI_DIST
		// Flush Rx FIFO
		SPI0_MCR |= SPI_MCR_CLR_RXF;
#else
		// Reset bit position
		distance_bit_pos = 0;
#endif

		// Reset the PWM counter as we aren't quite sure where the internal counter is right now
		FTM0_CNT = 0; // Reset counter

		// Set FTM to PWM output - Edge Aligned, High-true pulses
		FTM0_C7SC = 0x28; // MSnB:MSnA = 10, ELSnB:ELSnA = 01
	}

	distance_pulses_on = !distance_pulses_on;

	// Clear the interrupt
	PIT_TFLG0 = PIT_TFLG_TIF;
}

inline void distance_setup()
{
	// Setup distance read parameters for iGaging Distance Scale
	//       freq = 9kHz
	// duty_cycle = 20%
	// high_delay = (1/freq) *       (duty_cycle/100)
	//  low_delay = (1/freq) * ((100-duty_cycle)/100)

	// Setup PWM source
	SIM_SCGC6 |= SIM_SCGC6_FTM0;

	// Disable write protect and allow access to all the registers
	FTM0_CNT = 0; // Reset counter

	// Set FTM to PWM output - Edge Aligned, High-true pulses
	FTM0_C7SC = 0x28; // MSnB:MSnA = 10, ELSnB:ELSnA = 01

	// System clock, /w prescalar setting of 0
	FTM0_SC = FTM_SC_CLKS(1) | FTM_SC_PS(0);

	// PWM Period
	// 48 MHz / 9 kHz = 5333.333 or 0x14d5
	FTM0_MOD = 0x14d5;

	// Clock source for iGaging calipers
	FTM0_C7V = 0x042A; // 0x14d5 * 0.20 = 1066.6 or 0x42A
	PORTD_PCR7 = PORT_PCR_SRE | PORT_PCR_DSE | PORT_PCR_MUX(4) | PORT_PCR_PE;

	// Indicate that pulses have been enabled
	distance_pulses_on = 1;


	// Setup PIT (Programmable Interrupt Timer)
	SIM_SCGC6 |= SIM_SCGC6_PIT;;
	PIT_MCR = 0x00; // Enable module, do not freeze timers in debug mode

	// Timer Count-down value
	// (48 MHz / 9 KHz) * 21 cycles = 112 000 ticks (0x1B580) | Not long enough due to delays
	//                    Halfway   = 114 667 ticks (0x1BFEB)
	// (48 MHz / 9 KHz) * 22 cycles = 117 333 ticks (0x1CA55) | Sometimes too long due to delays
	// (48 MHz / 9 KHz) * 23 cycles = 122 667 ticks (0x1DF2A)
	PIT_LDVAL0 = 0x1BFEB;
	//PIT_LDVAL0 = 0x2DC6C00; // Once per second

	// Enable Timer, Enable interrupt
	PIT_TCTRL0 = PIT_TCTRL_TIE | PIT_TCTRL_TEN;

	// Enable PIT Ch0 interrupt
	NVIC_ENABLE_IRQ( IRQ_PIT_CH0 );

	// Set PIT0 interrupt to 2nd highest priority
	NVIC_SET_PRIORITY( IRQ_PIT_CH0, 1 );


#ifdef SPI_DIST
	// Setup SPI interface to work as a running shift register
	// Enable SPI internal clock
	SIM_SCGC6 |= SIM_SCGC6_SPI0;

	// Setup MOSI (SIN) and SCLK (SCK)
	PORTC_PCR7 = PORT_PCR_DSE | PORT_PCR_MUX(2) | PORT_PCR_PFE | PORT_PCR_PE | PORT_PCR_PS;
	PORTD_PCR1 = PORT_PCR_DSE | PORT_PCR_MUX(2) | PORT_PCR_PFE | PORT_PCR_PE;

	// Setup SS (PCS)
	PORTC_PCR4 = PORT_PCR_DSE | PORT_PCR_MUX(2) | PORT_PCR_PE;

	// Slave Mode
	// Inactive chip state PCS0 is high
	SPI0_MCR = SPI_MCR_PCSIS(1) | SPI_MCR_CONT_SCKE;

	// DSPI Clock and Transfer Attributes
	// Frame Size: 21 bits or 7 * 3
	// CPHA Leading edge, following capture
	// CLK Low by default
	SPI0_CTAR0_SLAVE = SPI_CTAR_FMSZ(6) | SPI_CTAR_CPHA;
#else
	// GPIO Interrupt Bit-Bang

	// Distance Sensor Pin Setup
	// Enable pins
	GPIOC_PDDR &= ~(1<<7); // Input Data
	GPIOD_PDDR &= ~(1<<1); // Input Clk

	PORTC_PCR7 = PORT_PCR_PFE | PORT_PCR_MUX(1) | PORT_PCR_PE | PORT_PCR_PS;
	PORTD_PCR1 = PORT_PCR_PFE | PORT_PCR_MUX(1) | PORT_PCR_PE | PORT_PCR_IRQC(10); // Falling edge detect

	// Enable IRQ
	NVIC_ENABLE_IRQ( IRQ_PORTD );

	// Set PORTD interrupt to 3rd highest priority
	NVIC_SET_PRIORITY( IRQ_PORTD, 2 );
#endif
}



// ------ Force Measurement ------

// TODO may need some sort of synchronization between full reads
volatile uint8_t uart0_pos = 0;
volatile uint8_t overforce = 0;
void uart0_status_isr()
{
	// UART0_S1 must be read for the interrupt to be cleared
	if ( UART0_S1 & ( UART_S1_RDRF | UART_S1_IDLE ) )
	{
		uint8_t available = UART0_RCFIFO;

		// If there was actually nothing
		if ( available == 0 )
		{
			// Cleanup
			available = UART0_D;
			UART0_CFIFO = UART_CFIFO_RXFLUSH;
			goto done;
		}

		// Read UART0 into buffer until FIFO is empty
		while ( available-- > 0 )
		{
			Main_FreeRunData.force_serial[uart0_pos] = UART0_D;

			if ( Main_FreeRunData.force_serial[uart0_pos++] == '\r' )
			{
				// Remove final CR
				Main_FreeRunData.force_serial[uart0_pos - 1] = 0x00;

				uart0_pos = 0;

				/* XXX Old force limiting, too slow for solid objects, use ADC
				// Check the MSD to make sure we haven't exceeded 600g
				// XXX The gauge is designed to work +/- 500g with 200% overload capacity
				// To be safe, checking for 6,7,8,9 in the MSD (this is then 2nd character)
				// Format Example +123.4KTO
				// This is not a full string comparison because this needs to be detected extremely quickly in case of problems
				switch ( Main_FreeRunData.force_serial[1] )
				{
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					// STOP Motor, and make a noise
					motor_up_start();
					buzzer_set( 1000 );
					overforce = 1;
					warn_msg("Over force! - ");
					dPrint( (char*)Main_FreeRunData.force_serial );
					print(" ");
					printInt16( Main_FreeRunData.force_adc );
					print( NL );
					break;

				default:
					// Disable buzzer
					if ( overforce )
					{
						overforce = 0;
						buzzer_set( 4095 );
					}
					break;
				}
				*/

				// Queue the next command
				UART0_D = 'D';
				UART0_D = '\r';
			}
		}
	}

done:
	return;
}

// Send each character from the string (except the NULL), waiting until each character is sent before proceeding
// NOTE: There is no delay after sending the last character
//       If first character is NULL, it will be sent
inline uint8_t force_serial_tx( char* str )
{
	// Only use if free running serial mode is not enabled
	if ( Main_ForceCaptureMode & ForceCaptureMode_FreeRun_Serial )
		return 0;

	// Loop through string
	// XXX Does not take into account the Tx FIFO size
	//     Shouldn't be an issue as most commands are only 2 bytes long
	while ( *str != '\0' )
	{
		UART0_D = *str++; // Send dataword (character)
	}

	return 1;
}

// Wait until FIFO has a character (no timeout...)
inline uint8_t force_serial_rx( char* data, uint16_t timeout )
{
	// Only use if free running serial mode is not enabled
	if ( Main_ForceCaptureMode & ForceCaptureMode_FreeRun_Serial )
		return 0;

	// Wait for a response (something to arrive in the FIFO)
	uint32_t cur_time = systick_millis_count;
	while ( UART0_SFIFO & UART_SFIFO_RXEMPT )
	{
		// Check for timeout
		if ( systick_millis_count - cur_time >= timeout )
		{
			return 2;
		}
	}

	// Read from RX FIFO
	*data = UART0_D;

	return 1;
}

// Send and wait for response from the force gauge
uint8_t force_serial_cmd( char cmd, char* data, uint8_t data_len, uint16_t timeout )
{
	// Write command to data register
	//  NOTE: Imada only uses single byte commands, this makes it easy
	//        Always followed by a '\r' CR
	char outCmd[] = { cmd, '\r', 0x00 };
	if ( !force_serial_tx( outCmd ) )
		goto force_error;

	uint8_t pos;
retry:
	pos = 0;

	// Make sure there's enough room in the buffer
	while ( pos < data_len )
	{
		// Check for a timeout
		if ( force_serial_rx( &data[pos], timeout ) == 2 )
		{
			goto force_timeout;
		}

		// Check for a CR (final output character of a response)
		if ( data[pos++] == '\r' )
		{
			// Remove final CR
			data[pos - 1] = 0x00;
			return 0;
		}
	}
	goto force_space;

force_space:
	warn_print("Serial buffer room exceeded...");
	goto retry;

force_timeout:
	warn_print("Serial command timeout...");
	goto force_error;

force_error:
	erro_print("Problem sending force gauge command: ");
	char tmpStr[] = { cmd, 0x00 };
	dPrint( tmpStr );
	print(" (");
	printHex( outCmd[0] );
	print(" ");
	printHex( outCmd[1] );
	print(")" NL);

	UART0_CFIFO |= UART_CFIFO_TXFLUSH | UART_CFIFO_RXFLUSH;

	return 0;
}

inline void force_setup()
{
	// Setup force gauge
	// Two methods are used to collect data
	// 1) Serial/RS232 input (calibrated)
	// 2) Analog (uncalibrated)
	//
	// RS232 runs rather slowly, while the ADCs can be set to free-running
	// Periodically retrieving serial data means that the more accurate/faster ADC data can be calibrated correctly
	// Rather than doing the correlation immediately, both datapoints are used

	// The following force gauges are supported (and likely any force gauge in that family)
	// * DS2-1
	// * DPS-1R
#define DS2_1
#ifdef DPS_1R
	// DPS-1R - Uses a much slower baud rate
	// Setup baud rate - 2400 Baud
	// 48 MHz / ( 16 * Baud ) = BDH/L
	// Baud: 2400 -> 48 MHz / ( 16 * 2400 ) = 1250
	// Thus baud setting = 1250
	// NOTE: If finer baud adjustment is needed see UARTx_C4 -> BRFA in the datasheet
	uint16_t baud = 1250; // Max setting of 8191
#elif defined(DS2_1)
	// DS2-1 - Uses a more modern baud rate
	// Setup baud rate - 19200 Baud
	// 48 MHz / ( 16 * Baud ) = BDH/L
	// Baud: 19200 -> 48 MHz / ( 16 * 19200 ) = 156.25
	// Thus baud setting = 156
	uint16_t baud = 156; // Max setting of 8191
#else
	erro_print("Force gauge not set...please recompile.");
#endif

	// Setup the the UART interface for keyboard data input
	SIM_SCGC4 |= SIM_SCGC4_UART0; // Disable clock gating

	// Pin Setup for UART0
	PORTB_PCR16 = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3); // RX Pin
	PORTB_PCR17 = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3); // TX Pin

	UART0_BDH = (uint8_t)(baud >> 8);
	UART0_BDL = (uint8_t)baud;

	// 8 bit, No Parity, Idle Character bit after stop
	// NOTE: For 8 bit with Parity you must enable 9 bit transmission (pg. 1065)
	//       You only need to use UART0_D for 8 bit reading/writing though
	// UART_C1_M UART_C1_PE UART_C1_PT UART_C1_ILT
	UART0_C1 = UART_C1_ILT;

	// Number of bytes in FIFO before TX Interrupt
	UART0_TWFIFO = 1;

	// Number of bytes in FIFO before RX Interrupt
	UART0_RWFIFO = 1;

	// TX FIFO Enabled, RX FIFO Enabled
	// UART_PFIFO_TXFE UART_PFIFO_RXFE
	UART0_C2 = 0; // Has to be cleared before setting PFIFO (see docs)
	UART0_PFIFO = UART_PFIFO_RXFE | UART_PFIFO_TXFE;

	// TX/RX FIFO Size:
	//  0x0 - 1 dataword
	//  0x1 - 4 dataword
	//  0x2 - 8 dataword
	//   etc. (see docs)
#ifdef FIFO_DEBUG
	dbug_msg("FIFO Sizes TX: ");
	printHex( UART0_PFIFO & 0x70 >> 4 );
	print(" RX: ");
	printHex( UART0_PFIFO & 0x07 );
	print( NL );
#endif

	// Reciever Inversion Disabled, LSBF
	// UART_S2_RXINV UART_S2_MSBF
	UART0_S2 |= 0x00;

	// Transmit Inversion Disabled
	// UART_C3_TXINV
	UART0_C3 |= 0x00;

	// TX Enabled, RX Enabled
	// UART_C2_TE UART_C2_RE UART_C2_RIE
	UART0_C2 = UART_C2_RE | UART_C2_TE;

	// Add interrupt to the vector table
	NVIC_ENABLE_IRQ( IRQ_UART0_STATUS );

	// Zero out serial display
	char commandOut[10];
	force_serial_cmd( 'Z', commandOut, sizeof( commandOut ), 1000 );


	// ---- ADC Setup ----

	// Setup VREF to 1.2 V
	VREF_TRM = 0x60;
	VREF_SC = 0xE1; // Enable 1.2 volt ref

	// Enable ADC clock
	SIM_SCGC6 |= SIM_SCGC6_ADC0;

	// Make sure calibration has stopped
	ADC0_SC3 = 0;

	// - CFG1 -
	// ADIV:   (input)/2 divider
	// ADICLK:   (bus)/2 divider
	// MODE:   16-bit
	// ADLSMP: Long sample
	ADC0_CFG1 = ADC_CFG1_ADIV(1) | ADC_CFG1_ADICLK(1) | ADC_CFG1_MODE(3) | ADC_CFG1_ADLSMP;

	// - CFG2 -
	// ADLSTS: 6 extra ADCK cycles; 10 ADCK cycles total sample time
	ADC0_CFG2 = ADC_CFG2_ADLSTS(2);

	// - SC2 -
#if defined(VREF3_3)
	// REFSEL: Use default 3.3V reference
	ADC0_SC2 = ADC_SC2_REFSEL(0);
#elif defined(VREF1_2)
	// REFSEL: Use 1.2V reference, see VREF setup above
	ADC0_SC2 = ADC_SC2_REFSEL(1);
#endif

	// - SC3 -
	// CAL:  Start calibration
	// AVGE: Enable hardware averaging
	// AVGS: 32 samples averaged
	// 32 sample averaging
	ADC0_SC3 = ADC_SC3_CAL | ADC_SC3_AVGE | ADC_SC3_AVGS(3);

	// Wait for calibration
	while ( ADC0_SC3 & ADC_SC3_CAL );

	// Apply computed calibration offset
	// XXX Note, for single-ended, only the plus side offsets have to be applied
	//     For differential the minus side also has to be set as well

	__disable_irq(); // Disable interrupts while reading/setting offsets

	// Set calibration
	// ADC Plus-Side Gain Register
	// See Section 31.4.7 in the datasheet (mk20dx256vlh7) for details
	uint16_t sum = ADC0_CLPS + ADC0_CLP4 + ADC0_CLP3 + ADC0_CLP2 + ADC0_CLP1 + ADC0_CLP0;
	sum = (sum / 2) | 0x8000;
	ADC0_PG = sum;

	__enable_irq(); // Re-enable interrupts

	// Start ADC reading loop
	// - SC1A -
	// ADCH: Channel DAD0 (A10)
	// AIEN: Enable interrupt
	ADC0_SC1A = ADC_SC1_AIEN | ADC_SC1_ADCH(0);

	// Enable ADC0 IRQ Vector
	NVIC_ENABLE_IRQ( IRQ_ADC0 );
}

void adc0_isr()
{
	// Check if ADC data is ready
	if ( (ADC0_SC1A & ADC_SC1_COCO) )
	{
		//uint16_t prev_force = Main_FreeRunData.force_adc;
		Main_FreeRunData.force_adc = ADC0_RA;

		// Check to see if force has exceeded limit
#if defined(VREF1_2)
		uint16_t limit = 60000;
#elif defined(VREF3_3)
		uint16_t limit = 23000;
#else
#error "Define a VREF, or else the load cell will die..."
#endif

		// Check for force limit
		Direction prev_dir = Main_FreeRunData.direction;
		if ( Main_FreeRunData.force_adc > limit )
		{
			// Reverse motor first
			motor_up_start();

			// Check if currently running a test
			// On the first run we need to find the limit, so this is the signal to reverse directions
			if ( Main_TestState.running )
			{
#define BOTTOM_LIMIT_OFFSET 5
				// Set current distance - (offset) as bottom
				if ( prev_dir == Direction_Down )
				{
					Main_TestState.distance_bottom = Main_FreeRunData.distance - BOTTOM_LIMIT_OFFSET;
					info_msg("(Max Force) Setting min distance to '");
					printInt32( Main_TestState.distance_bottom );
					print("' ADC Force: ");
					printInt16( Main_FreeRunData.force_adc );
					print( NL );
				}
			}
			else
			{
				// Make a noise if not a test
				buzzer_set( 1000 );
				overforce = 1;

				warn_msg("Over force! - ");
				dPrint( (char*)Main_FreeRunData.force_serial );
				print(" ");
				printInt16( Main_FreeRunData.force_adc );
				print( NL );
			}
		}
		else
		{
			/* XXX Not ideal, should probably keep track of data a different way for this -HaaTa
			// Running a test, keep track of the min/max force for optimal distance start/stops
			// Using distance limits is faster and better for the load sensor
			if ( Main_TestState.running )
			{
				// Set max height using last useful force position
				if ( prev_force == Main_TestState.adc_origin && Main_FreeRunData.force_adc > Main_TestState.adc_origin )
				{
#define TOP_LIMIT_OFFSET 5
					// Set current distance + (offset) as top
					Main_TestState.distance_top = Main_FreeRunData.distance + TOP_LIMIT_OFFSET;
				}
			}
			*/

			// Disable buzzer
			if ( overforce )
			{
				overforce = 0;
				buzzer_set( 4095 );
			}
		}

		// Set ADC Max value
		if ( Main_FreeRunData.force_adc > Main_FreeRunData.force_adc_max )
			Main_FreeRunData.force_adc_max = Main_FreeRunData.force_adc;
	}

	// Enable the next interrupt
	// - SC1A -
	// ADCH: Channel DAD0 (A10)
	// AIEN: Enable interrupt
	ADC0_SC1A = ADC_SC1_AIEN | ADC_SC1_ADCH(0);
}



// ------ Buzzer ------

void buzzer_setup()
{
	// DAC Setup
	SIM_SCGC2 |= SIM_SCGC2_DAC0;

	// Set DAC to max, disables Buzzer
	*(int16_t *) &(DAC0_DAT0L) = 4095;

	// Enable DAC
	DAC0_C0 = DAC_C0_DACEN | DAC_C0_DACRFS; // 3.3V VDDA is DACREF_2
}

void buzzer_set( uint16_t val )
{
	if ( val <= 4095 )
	{
		*(int16_t *) &(DAC0_DAT0L) = val;
	}
	else
	{
		warn_print("DAC only supports values 0 to 4095");
	}
}



// ------ Continuity ------

void portd_isr()
{
	// Check each of the interrupts and clear them
	if ( PORTD_PCR6 & PORT_PCR_ISF )
	{
		// Read current level
		Main_FreeRunData.continuity = GPIOD_PDIR & (1<<6) ? 1 : 0;

		// Clear interrupt
		PORTD_PCR6 |= PORT_PCR_ISF;
	}

	// Distance bit-bang
	if ( PORTD_PCR1 & PORT_PCR_ISF )
	{
#ifdef DIST_DEBUG
		print( GPIOC_PDIR & (1<<7) ? "1" : "0" );
#endif
		distance_read_data |= GPIOC_PDIR & (1<<7) ? (1 << distance_bit_pos) : 0;
		distance_bit_pos++;

		// Finished building variable
		if ( distance_bit_pos >= 21 )
		{
#ifdef DIST_DEBUG
			print(" ");
			printInt32( distance_read_data );
			print(NL);
#endif
			// Check if we need to re-calibrate
			if ( distance_zero_next )
			{
#define DISTANCE_22BIT_MAX 0x1FFFFF
				// Check for negative of positive offset to start
				// Apply additive offset
				if ( distance_read_data < DISTANCE_22BIT_MAX / 2 )
				{
					distance_offset = DISTANCE_22BIT_MAX;
					distance_offset_enable = 1;
				}
				// Apply subtractive offset
				// TODO there's a bug here -HaaTa
				else
				{
					distance_offset = -DISTANCE_22BIT_MAX;
					distance_offset_enable = 1;
				}

				// Finished calibrating offset
				distance_zero_next = 0;
			}

			// Check if offset needs to be enabled/disabled
			if (
				( Main_FreeRunData.distance_raw > distance_read_data + DISTANCE_22BIT_MAX / 2 ) ||
				( Main_FreeRunData.distance_raw + DISTANCE_22BIT_MAX / 2 < distance_read_data )
			)
			{
				// Toggle
				distance_offset_enable = !distance_offset_enable;
			}

			// Set the new distance
			Main_FreeRunData.distance_raw = distance_read_data;
			Main_FreeRunData.distance = distance_read_data + (distance_offset_enable ? distance_offset : 0 );

			// Check current distance if this is a test to see if it's at a limit
			if ( Main_TestState.running )
			{
				switch ( Main_FreeRunData.direction )
				{
				// Going up
				case Direction_Up:
					// Check if we're at/past the max height
					if ( Main_FreeRunData.distance >= Main_TestState.distance_top )
					{
						// Increment current test and check to see if the test is finished
						if ( ++Main_TestState.test_cur < Main_TestState.test_total )
						{
							// Display message
							info_msg("(Top Height) Starting test '");
							printInt16( Main_TestState.test_cur );
							print("' Max adc force on prev: ");
							printInt16( Main_FreeRunData.force_adc_max );
							print( NL );

							// Reset max adc
							Main_FreeRunData.force_adc_max = 0;

							// Start going down
							motor_down_start();
						}
						else
						{
							// Display message
							info_msg("(Top Height) Test Complete; Max adc force on prev: ");
							printInt16( Main_FreeRunData.force_adc_max );
							print( NL );

							// Stop test
							Main_TestState.running = 0;
						}
					}
					break;

				// Going down
				case Direction_Down:
					// Check if we're at/past the minimum height
					if ( Main_FreeRunData.distance <= Main_TestState.distance_bottom )
					{
						// Display message
						info_print("(Bottom Height) going back up.");

						// Start going up
						motor_up_start();
					}
					break;
				}
			}

			// Reset data
			distance_read_data = 0;
		}

		// Clear interrupt
		PORTD_PCR1 |= PORT_PCR_ISF;
	}
}

void continuity_setup()
{
	// Continuity Tester Pin Setup
	// Goal is to detect voltage high
	// 21 - D6 - Sense
	// TODO confirm pin
	GPIOD_PDDR &= ~(1<<6); // Input

	// Pulldown resistor, interrupt on either edge, passive input filter
	PORTD_PCR6 = PORT_PCR_PFE | PORT_PCR_PE | PORT_PCR_MUX(1) | PORT_PCR_IRQC(11);

	// Enable IRQ
	NVIC_ENABLE_IRQ( IRQ_PORTD );
}

uint8_t continuity_read( uint8_t *data )
{
	*data = GPIOD_PDIR & (1<<6) ? 1 : 0;
	return 1;
}



// ------ Motor Control ------

void motor_stop()
{
	// Unset direction
	Main_FreeRunData.direction = Direction_None;

	// Pull high to stop motors
	GPIOC_PSOR |= (1 << 8);
	GPIOC_PSOR |= (1 << 9);
}

void motor_up_start()
{
	// First disable motor
	motor_stop();

	// Then check if limit switch is enabled
	if ( !( GPIOC_PDIR & (1 << 10) ) )
	{
		print( NL );
		erro_print("Upper limit switch triggered.");
		return;
	}

	// Set direction
	Main_FreeRunData.direction = Direction_Up;

	GPIOC_PCOR |= (1 << 8);
}

void motor_down_start()
{
	// First disable motor
	motor_stop();

	// Then check if limit switch is enabled
	if ( !( GPIOE_PDIR & (1 << 0) ) )
	{
		print( NL );
		erro_print("Lower limit switch triggered.");
		return;
	}

	// Set direction
	Main_FreeRunData.direction = Direction_Down;

	GPIOC_PCOR |= (1 << 9);
}

void portc_isr()
{
	// Check each of the interrupts and clear them
	if ( PORTC_PCR10 & PORT_PCR_ISF )
	{
		// Always stop first regardless
		Direction prev_dir = Main_FreeRunData.direction;
		motor_stop();

		// Check to see if running a test
		if ( Main_TestState.running && prev_dir == Direction_Up )
		{
			// Increment current test and check to see if the test is finished
			if ( Main_TestState.test_cur < Main_TestState.test_total )
			{
				// Display message
				Main_TestState.test_cur++;
				info_msg("(Upper Limit Switch) Starting test '");
				printInt16( Main_TestState.test_cur );
				print("' Max adc force on prev: ");
				printInt16( Main_FreeRunData.force_adc_max );
				print( NL );

				// Reset max adc
				Main_FreeRunData.force_adc_max = 0;

				// Start going down
				motor_down_start();
			}
			else
			{
				// Display message
				info_msg("(Upper Limit Switch) Test Complete; Max adc force on prev: ");
				printInt16( Main_FreeRunData.force_adc_max );
				print( NL );

				// Stop test
				Main_TestState.running = 0;
			}
		}
		// Normal limit switches
		else
		{
			// Check if going down, no reason to fire limit switch
			if ( prev_dir == Direction_Down )
			{
				motor_down_start();
			}
			else
			{
				warn_print("Upper Limit Switch!");
			}
		}
		PORTC_PCR10 |= PORT_PCR_ISF;
	}
}

void porte_isr()
{
	// Check each of the interrupts and clear them
	if ( PORTE_PCR0 & PORT_PCR_ISF )
	{
		// Always stop first regardless
		Direction prev_dir = Main_FreeRunData.direction;
		motor_stop();

		// Check to see if running a test
		if ( Main_TestState.running && prev_dir == Direction_Down )
		{
			// Display message
			info_print("(Lower Limit Switch) going back up.");

			// Start going up
			motor_up_start();
		}
		// Normal limit switches
		else
		{
			// Check if going up, no reason to fire limit switch
			if ( prev_dir == Direction_Up )
			{
				motor_up_start();
			}
			else
			{
				warn_print("Lower Limit Switch!");
			}
		}
		PORTE_PCR0 |= PORT_PCR_ISF;
	}
}

// Limit switch interrupt setup
// Should be the highest level interrupt (to avoid having the test stand destroy itself)
inline void limit_switch_setup()
{
	// TODO Decide on which pins to use
	// Currently PTC10 and PTE0

	// Enable GPIO, Interrupt on falling edge, Passive input filter, Pullups
	PORTC_PCR10 = PORT_PCR_MUX(1) | PORT_PCR_IRQC(10) | PORT_PCR_PFE | PORT_PCR_PE | PORT_PCR_PS;
	PORTE_PCR0  = PORT_PCR_MUX(1) | PORT_PCR_IRQC(10) | PORT_PCR_PFE | PORT_PCR_PE | PORT_PCR_PS;

	// Set GPIO as input
	GPIOC_PDIR |= (1 << 10);
	GPIOE_PDIR |= (1 << 0);

	// Enable IRQ
	NVIC_ENABLE_IRQ( IRQ_PORTC );
	NVIC_ENABLE_IRQ( IRQ_PORTE );

	// Set IRQ has highest priority
	NVIC_SET_PRIORITY( IRQ_PORTC, 0 );
	NVIC_SET_PRIORITY( IRQ_PORTE, 0 );
}

inline void motor_control_setup()
{
	// TODO Decide on which pins to use
	// Currently PTC8 and PTC9

	// Stop motor
	motor_stop();

	// Set GPIO as output pins
	GPIOC_PDDR |= (1 << 8);
	GPIOC_PDDR |= (1 << 9);

	// Enable GPIO, slow slew rate, high drive strength
	PORTC_PCR8 = PORT_PCR_SRE | PORT_PCR_DSE | PORT_PCR_MUX(1);
	PORTC_PCR9 = PORT_PCR_SRE | PORT_PCR_DSE | PORT_PCR_MUX(1);
}



// ------ Timer ------

inline void timer_setup()
{
	// Chained timers to create a 32 bit microsecond counter
	// This is only used to determine the time between readings rather than "timestamping"

	// Setup PIT (Programmable Interrupt Timer)
	SIM_SCGC6 |= SIM_SCGC6_PIT;;
	PIT_MCR = 0x00; // Enable module, do not freeze timers in debug mode

	// Timer Count-down value
	// (48 MHz / 1 MHz) = 48 ticks (0x30)
	PIT_LDVAL3 = 0x30;
	PIT_LDVAL2 = 0xFFFFFFFF;

	// Chain Timers
	PIT_TCTRL3 = PIT_TCTRL_CHN;

	// Enable Timers
	PIT_TCTRL3 |= PIT_TCTRL_TEN;
	PIT_TCTRL2 |= PIT_TCTRL_TEN;
}

inline uint32_t timer_timestamp()
{
	return 0xFFFFFFFF - PIT_CVAL2;
}

void timer_query()
{
	info_msg("20.833 ns ticks: ");
	printInt32( PIT_CVAL3 );
	print(" us: ");
	printInt32( 0xFFFFFFFF - PIT_CVAL2 );
}



// ------ RawIO Processing ------

void rawio_process()
{
	// Retrieve RawIO buffer(s)
	while ( Output_rawio_availablechar() )
	{
		info_print("RawIO Input Buffer: ");
		char buf[65];
		Output_rawio_getbuffer( buf );
		buf[64] = '\0'; // Just in case
		dPrint( buf );
	}

	// TODO setup modes

	// Send current status
	Main_FreeRunData.time = timer_timestamp();
	Output_rawio_sendbuffer( (char*)&Main_FreeRunData );
}



// ------ Zeroing -----

void zero()
{
	// Force Gauge
	info_print("Force Gauge");
	char commandOut[10];
	force_serial_cmd( 'Z', commandOut, sizeof( commandOut ), 1000 );
	if ( commandOut[0] != 'R' )
	{
		erro_print("Unsuccessful...");
	}

	// Distance
	info_print("Distance");
	distance_zero_next = 1;

	// Free-running data structure
	info_print("Data");
	Main_FreeRunData = (ForceCurveDataPoint){ 0 } ;
}



// ------ Main ------

int main()
{
	// Enable CLI
	CLI_init();

	// Register Force Gauge CLI dictionary
	CLI_registerDictionary( forceGaugeCLIDict, forceGaugeCLIDictName );

	// Setup
	distance_setup();
	limit_switch_setup();
	motor_control_setup();
	force_setup();
	continuity_setup();
	timer_setup();
	buzzer_setup();


	/* XXX Causes ADC corruption, do not enable initially
	// Enable Serial Force - Free Run
	// XXX This is to prevent damage to the force gauge so it can at least force the motor to stop
	//     if run by accident
	UART0_C2 |= UART_C2_RIE; // Set UART Rx interrupt
	// Queue serial read
	UART0_D = 'D';
	UART0_D = '\r';
	Main_ForceCaptureMode |= ForceCaptureMode_FreeRun_Serial;
	*/


	// Setup Modules
	Output_setup();

	// Main Detection Loop
	while ( 1 )
	{
		// Process CLI
		CLI_process();

		// RawIO Processing
		rawio_process();
	}
}

// TODO Removeme once implemented in Python
#if 0
void cliFunc_distRead( char* args )
{
	// Parse number from argument
	//  NOTE: Only first argument is used
	char* arg1Ptr;
	char* arg2Ptr;
	argumentIsolation_cli( args, &arg1Ptr, &arg2Ptr );

	// Convert the argument into an int
	int read_count = decToInt( arg1Ptr ) + 1;

	// If no argument specified, default to 1 read
	if ( *arg1Ptr == '\0' )
	{
		read_count = 2;
	}

	// Repeat reading as many times as specified in the argument
	print( NL );
	while ( --read_count > 0 )
	{
		// Prepare to print output
		info_msg("Distance: ");

		// Data
		uint32_t distInput = readDistanceGauge() - distanceOffset;

		// Output result
		printInt32( distInput );

		// Convert to mm
		// As per http://www.shumatech.com/web/21bit_protocol?page=0,1
		// 21 bits is 2560 CPI (counts per inch) (C/inch)
		// 1 inch is 25.4 mm
		// 2560 / 25.4 = 100.7874016... CPMM (C/mm)
		// Or
		// 1 count is 1/2560 = 0.000390625... inches
		// 1 count is (1/2560) * 25.4 = 0.00992187500000000 mm = 9.92187500000000 um = 9921.87500000000 nm
		// Since there are 21 bits (2 097 152 positions) converting to um is possible by multiplying by 1000
		//    which is 2 097 152 000, and within 32 bits (4 294 967 295).
		// However, um is still not convenient, so 64 bits (18 446 744 073 709 551 615) is a more accurate alternative.
		// For each nm there are 2 097 152 000 000 positions.
		// And for shits:
		//    mm is 2 097 152                 :          0.009 921 875 000 mm : 32 bit
		//    um is 2 097 152 000             :          9.921 875 000     um : 32 bit (ideal acc. for 32 bit)
		//    nm is 2 097 152 000 000         :      9 921.875 000         nm : 64 bit
		//    pm is 2 097 152 000 000 000     :  9 921 875.000             pm : 64 bit (ideal acc. for 64 bit)

		// XXX Apparently shumatech was sorta wrong about the 21 bits of usage
		// Yes there are 21 bits, but the values only go from ~338 to ~30681 which is less than 16 bits...
		// This means that the conversion at NM can use 32 bits :D
		// It's been noted that the multiplier should be 100.6 (and that it could vary from scale to scale)
		uint32_t distNM = distInput * 9921;;
		uint32_t distUM = distNM / 1000;
		uint32_t distMM = distUM / 1000;

		print("  ");
		printInt32( distMM );
		print(" mm  ");
		printInt32( distUM );
		print(" um  ");
		printInt32( distNM );
		print(" nm  ");

		print( NL );

		// Only delay if still counting
		if ( read_count > 1 )
			delay( 50 );
	}
}
#endif

void cliFunc_up( char* args )
{
	motor_up_start();
}

void cliFunc_down( char* args )
{
	motor_down_start();
}

void cliFunc_stop( char* args )
{
	motor_stop();

	// Stop test if running
	if ( Main_TestState.running )
	{
		info_print("Stopping test.");
		Main_TestState.running = 0;
	}
}

void cliFunc_stat( char* args )
{
	print( NL );

	// Timestamp
	printInt32( Main_FreeRunData.time );

	// Distance
	print("|");
	printInt32( Main_FreeRunData.distance_raw );
	print(":");
	printInt32( Main_FreeRunData.distance );

	// Force Serial
	print("|");
	// Query the serial interface
	if ( !( Main_ForceCaptureMode & ForceCaptureMode_FreeRun_Serial ) )
	{
		force_serial_cmd( 'D', (char*)Main_FreeRunData.force_serial, sizeof( Main_FreeRunData.force_serial ), 1000 );
	}
	dPrint( (char*)Main_FreeRunData.force_serial );

	// Force ADC
	print("|");
	printInt16( Main_FreeRunData.force_adc );

	// Continuity/Sense
	print("|");
	printInt8( Main_FreeRunData.continuity );

	// Speed
	print("|");
	if ( !( Main_ForceCaptureMode & ForceCaptureMode_FreeRun_Speed ) )
	{
		// TODO
	}
	printInt16( Main_FreeRunData.speed );

	// Direction
	print("|");
	printInt8( Main_FreeRunData.direction );
}

void cliFunc_zero( char* args )
{
	print( NL );
	info_print("Zeroing...");

	zero();

	info_print("Zeroing Complete");
}

void cliFunc_free( char* args )
{
	char* curArgs;
	char* arg1Ptr;
	char* arg2Ptr = args;

	// Process argument
	curArgs = arg2Ptr;
	CLI_argumentIsolation( curArgs, &arg1Ptr, &arg2Ptr );

	// Check for special args
	switch ( *arg1Ptr )
	{
	case 'f':
	case 'F':
		UART0_C2 |= UART_C2_RIE; // Set UART Rx interrupt

		// Queue serial read
		UART0_D = 'D';
		UART0_D = '\r';

		Main_ForceCaptureMode = ForceCaptureMode_FreeRun_Full;
		return;

	// Default to off
	case 'o':
	case 'O':
	default:
		UART0_C2 &= ~(UART_C2_RIE); // Disable UART Rx interrupt

		Main_ForceCaptureMode = ForceCaptureMode_OnRequest;
		return;

	case 's':
	case 'S':
		UART0_C2 ^= UART_C2_RIE; // Toggle UART Rx interrupt

		if ( UART0_C2 & UART_C2_RIE )
		{
			// Queue serial read
			UART0_D = 'D';
			UART0_D = '\r';
		}

		Main_ForceCaptureMode ^= ForceCaptureMode_FreeRun_Serial;
		return;

	case 'p':
	case 'P':
		Main_ForceCaptureMode ^= ForceCaptureMode_FreeRun_Speed;
		return;
	}
}

void cliFunc_buzz( char* args )
{
	char* curArgs;
	char* arg1Ptr;
	char* arg2Ptr = args;

	// Process argument
	curArgs = arg2Ptr;
	CLI_argumentIsolation( curArgs, &arg1Ptr, &arg2Ptr );

	// Get argument if set
	uint16_t value = *arg1Ptr == '\0' ? 4095 : numToInt( arg1Ptr );

	// Set buzzer
	buzzer_set( value );

	print( NL );
	info_msg("Buzzer set to: ");
	printInt16( value );
}

void cliFunc_test( char* args )
{
	print( NL );

	// Check if a test is already running
	if ( Main_TestState.running )
	{
		warn_print("Test is already running, use 'stop' command to stop test.");
		return;
	}

	char* curArgs;
	char* arg1Ptr;
	char* arg2Ptr = args;

	// Process argument
	curArgs = arg2Ptr;
	CLI_argumentIsolation( curArgs, &arg1Ptr, &arg2Ptr );

	// Get argument if set
	Main_TestState.test_total = *arg1Ptr == '\0' ? 1 : numToInt( arg1Ptr );

	// Reset state
	Main_TestState.test_cur = 1; // Starts on test 1
	Main_TestState.distance_bottom = 0;
	Main_TestState.distance_top = Main_FreeRunData.distance;
	Main_TestState.adc_origin = Main_FreeRunData.force_adc;

	// Reset max adc
	Main_FreeRunData.force_adc_max = 0;

	// Run tests
	Main_TestState.running = 1;

	// Msg
	info_msg("Starting test with '");
	printInt16( Main_TestState.test_total );
	print("' iterations");

	// Start motor downwards
	motor_down_start();
}

