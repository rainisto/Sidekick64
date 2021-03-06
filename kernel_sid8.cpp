/*
  _________.__    .___      __   .__        __          _________.___________   
 /   _____/|__| __| _/____ |  | _|__| ____ |  | __     /   _____/|   \______ \  
 \_____  \ |  |/ __ |/ __ \|  |/ /  |/ ___\|  |/ /     \_____  \ |   ||    |  \ 
 /        \|  / /_/ \  ___/|    <|  \  \___|    <      /        \|   ||    `   \
/_______  /|__\____ |\___  >__|_ \__|\___  >__|_ \    /_______  /|___/_______  /
        \/         \/    \/     \/       \/     \/            \/             \/ 
 
 kernel_sid8.cpp

 RasPiC64 - A framework for interfacing the C64 and a Raspberry Pi 3B/3B+
          - Sidekick SID: a SID and SFX Sound Expander Emulation / SID-8 is a lazy modification to emulate 8 SIDs at once
		    (using reSID by Dag Lem)
 Copyright (c) 2019, 2020 Carsten Dachsbacher <frenetic@dachsbacher.de>

 Logo created with http://patorjk.com/software/taag/
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <math.h>
#include "kernel_sid8.h"
#ifdef COMPILE_MENU
#include "kernel_menu.h"
#include "launch.h"

static u32 launchPrg;
#endif

#undef USE_VCHIQ_SOUND

//                 _________.___________         ____                      ________    ______  ____________  
//_______   ____  /   _____/|   \______ \       /  _ \       ___.__. _____ \_____  \  /  __  \/_   \_____  \ 
//\_  __ \_/ __ \ \_____  \ |   ||    |  \      >  _ </\    <   |  |/     \  _(__  <  >      < |   |/  ____/ 
// |  | \/\  ___/ /        \|   ||    `   \    /  <_\ \/     \___  |  Y Y  \/       \/   --   \|   /       \ 
// |__|    \___  >_______  /|___/_______  /    \_____\ \     / ____|__|_|  /______  /\______  /|___\_______ \
//             \/        \/             \/            \/     \/          \/       \/        \/             \/
#include "resid/sid.h"
using namespace reSID;

static u32 CLOCKFREQ = 985248;	// exact clock frequency of the C64 will be measured at start up

// SID types and digi boost (only for MOS8580)
static unsigned int SID_MODEL[8] = { 8580, 8580, 8580, 8580, 8580, 8580, 8580, 8580 };
static unsigned int SID_DigiBoost[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// do not change this value
#define NUM_SIDS 8
static SID *sid[ NUM_SIDS ];

#ifdef EMULATE_OPL2
FM_OPL *pOPL;
u32 fmOutRegister;
#endif

// a ring buffer storing SID-register writes (filled in FIQ handler)
// TODO should be much smaller
#define RING_SIZE (1024*128)
static u32 ringBufGPIO[ RING_SIZE ];
static unsigned long long ringTime[ RING_SIZE ];
static u32 ringWrite;

// prepared GPIO output when SID-registers are read
static u32 outRegisters[ 32 ];

// counts the #cycles when the C64-reset line is pulled down (to detect a reset)
static u32 resetCounter,
		   resetPressed, resetReleased;

// this is the actual configuration of the emulation
extern u32 cfgRegisterRead;			// don't use this when you have a SID(-replacement) in the C64
extern u32 cfgEmulateOPL2;
extern u32 cfgSID2_Disabled;
extern u32 cfgSID2_PlaySameAsSID1;
extern u32 cfgMixStereo;
extern u32 cfgSID2_Addr;
extern s32 cfgVolSID1_Left, cfgVolSID1_Right;
extern s32 cfgVolSID2_Left, cfgVolSID2_Right;
extern s32 cfgVolOPL_Left, cfgVolOPL_Right;

unsigned int SAMPLERATE = 44100;

//  __     __                __      ___                   ___ 
// /__` | |  \     /\  |\ | |  \    |__   |\/|    | |\ | |  |  
// .__/ | |__/    /~~\ | \| |__/    |     |  |    | | \| |  |  
//                                                            
void initSID8()
{
	resetCounter = 0;

	for ( int i = 0; i < NUM_SIDS; i++ )
	{
		sid[ i ] = new SID;

		for ( int j = 0; j < 24; j++ )
			sid[ i ]->write( j, 0 );

		// no mistake, take the model of the first for all 8
		if ( (SID_MODEL[ 0 ] % 3) == 6581 )
		{
			sid[ i ]->set_chip_model( MOS6581 );
		} else
		{
			sid[ i ]->set_chip_model( MOS8580 );
			if ( SID_DigiBoost[ 0 ] == 0 )
			{
				sid[ i ]->set_voice_mask( 0x07 );
				sid[ i ]->input( 0 );
			} else
			{
				sid[ i ]->set_voice_mask( 0x0f );
				sid[ i ]->input( -32768 );
			}
		}
	}

	// ring buffer init
	ringWrite = 0;
	for ( int i = 0; i < RING_SIZE; i++ )
		ringTime[ i ] = 0;
}

void quitSID8()
{
	for ( int i = 0; i < NUM_SIDS; i++ )
		delete sid[ i ];
}

static unsigned long long cycleCountC64;


#ifdef COMPILE_MENU
extern CLogger			*logger;
extern CTimer			*pTimer;
extern CScheduler		*pScheduler;
extern CInterruptSystem	*pInterrupt;
extern CVCHIQDevice		*pVCHIQ;
extern CScreenDevice	*screen;
static CSoundBaseDevice	*m_pSound;
#else

CLogger				*logger;
CTimer				*pTimer;
CScheduler			*pScheduler;
CInterruptSystem	*pInterrupt;
CVCHIQDevice		*pVCHIQ;
CScreenDevice		*screen;


boolean CKernel::Initialize( void )
{
	boolean bOK = TRUE;

#ifdef USE_HDMI_VIDEO
	if ( bOK ) bOK = m_Screen.Initialize();

	if ( bOK )
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice( m_Options.GetLogDevice(), FALSE );
		if ( pTarget == 0 )
			pTarget = &m_Screen;

		bOK = m_Logger.Initialize( pTarget );
		logger = &m_Logger;
	}
#endif

	if ( bOK ) bOK = m_Interrupt.Initialize();
	if ( bOK ) bOK = m_Timer.Initialize();

#ifdef USE_VCHIQ_SOUND
	if ( bOK ) bOK = m_VCHIQ.Initialize();
	pVCHIQ = &m_VCHIQ;
#endif

	pTimer = &m_Timer;
	pScheduler = &m_Scheduler;
	pInterrupt = &m_Interrupt;
	screen = &m_Screen;

	return bOK;
}
#endif

static u32 renderDone = 0;

static u32 vu_Mode = 0;
static u32 vu_nLEDs = 0;

#ifdef COMPILE_MENU
void KernelSIDFIQHandler8( void *pParam );

void KernelSIDRun8( CGPIOPinFIQ m_InputPin, CKernelMenu *kernelMenu, const char *FILENAME, bool hasData = false, u8 *prgDataExt = NULL, u32 prgSizeExt = 0 )
#else
void CKernel::Run( void )
#endif
{
// initialize ARM cycle counters (for accurate timing)
	initCycleCounter();

	// initialize GPIOs
	gpioInit();
	SET_BANK2_OUTPUT

	// initialize latch and software I2C buffer
	initLatch();
	latchSetClearImm( 0, LATCH_RESET | LATCH_LED_ALL | LATCH_ENABLE_KERNAL );

	SETCLR_GPIO( bNMI | bDMA | bGAME | bEXROM, 0 );

	#ifdef USE_OLED
	// I know this is a gimmick, but I couldn't resist ;-)
	splashScreen( raspi_sid_splash );
	#endif

	//	logger->Write( "", LogNotice, "initialize SIDs..." );
	initSID8();

	#ifdef COMPILE_MENU
	if ( FILENAME == NULL && !hasData )
	{
		launchPrg = 0;
		disableCart = 1;
	} else
	{
		launchPrg = 1;
		if ( launchGetProgram( FILENAME, hasData, prgDataExt, prgSizeExt ) )
			launchInitLoader( false ); else
			launchPrg = 0;
	}
	#endif

	//
	// setup FIQ
	//
	#ifdef COMPILE_MENU
	m_InputPin.ConnectInterrupt( KernelSIDFIQHandler8, kernelMenu );
	#else
	m_InputPin.ConnectInterrupt( this->FIQHandler, this );
	#endif
	m_InputPin.EnableInterrupt( GPIOInterruptOnRisingEdge );

#ifndef COMPILE_MENU

	latchSetClearImm( LATCH_RESET, LATCH_LED_ALL | LATCH_ENABLE_KERNAL );

	cycleCountC64 = 0;
	while ( cycleCountC64 < 10 ) 
	{
		pScheduler->MsSleep( 100 );
	}


	//
	// measure clock rate of the C64 (more accurate syncing with emulation, esp. for HDMI output)
	//
	cycleCountC64 = 0;
	unsigned long long startTime = pTimer->GetClockTicks();
	unsigned long long curTime;

	do {
		curTime = pTimer->GetClockTicks();
	} while ( curTime - startTime < 1000000 );

	unsigned long long clockFreq = cycleCountC64 * 1000000 / ( curTime - startTime );
	CLOCKFREQ = clockFreq;
	logger->Write( "", LogNotice, "Measured C64 clock frequency: %u Hz", (u32)CLOCKFREQ );
#endif

	for ( int i = 0; i < NUM_SIDS; i++ )
		sid[ i ]->set_sampling_parameters( CLOCKFREQ, SAMPLE_INTERPOLATE, SAMPLERATE );

	//
	// initialize sound output (either PWM which is output in the FIQ handler, or via HDMI)
	//
	initSoundOutput( &m_pSound, pVCHIQ );

//	logger->Write( "", LogNotice, "start emulating..." );
	cycleCountC64 = 0;
	unsigned long long nCyclesEmulated = 0;

	// how far did we consume the commands in the ring buffer?
	unsigned int ringRead = 0;

	#ifdef COMPILE_MENU
	// let's be very convincing about the caches ;-)
	for ( u32 i = 0; i < 20; i++ )
	{
		launchPrepareAndWarmCache();

		// FIQ handler
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)&FIQ_HANDLER, 3*1024 );
		FORCE_READ_LINEAR32( (void*)&FIQ_HANDLER, 3*1024 );
	}

	if ( !launchPrg )
		SETCLR_GPIO( bNMI | bDMA | bGAME | bEXROM, 0 );

	DELAY(10);
	latchSetClearImm( LATCH_RESET, LATCH_LED_ALL | LATCH_ENABLE_KERNAL );

	if ( launchPrg )
	{
		while ( !disableCart )
		{
			#ifdef COMPILE_MENU
			TEST_FOR_JUMP_TO_MAINMENU( cycleCountC64, resetCounter )
			#endif
			asm volatile ("wfi");
		}
	} 
	#endif

	resetCounter = cycleCountC64 = 0;
	nCyclesEmulated = 0;
	ringRead = 0;
	for ( int i = 0; i < NUM_SIDS; i++ )
		for ( int j = 0; j < 24; j++ )
			sid[ i ]->write( j, 0 );

	// new main loop mainloop
	while ( true )
	{
		#ifdef COMPILE_MENU
		//TEST_FOR_JUMP_TO_MAINMENU( cycleCountC64, resetCounter )
		if ( cycleCountC64 > 2000000 && resetCounter > 500000 ) {
				EnableIRQs();
				m_InputPin.DisableInterrupt();
				m_InputPin.DisconnectInterrupt();
				quitSID8();
				return;
			}
		#endif

		if ( resetCounter > 3 && resetReleased )
		{
			resetCounter = 0;

			for ( int i = 0; i < NUM_SIDS; i++ )
				for ( int j = 0; j < 24; j++ )
					sid[ i ]->write( j, 0 );
		}

	#ifdef USE_OLED
		if ( renderDone == 2 )
		{
			if ( !sendFramebufferDone() )
				sendFramebufferNext( 1 );		

			if ( sendFramebufferDone() )
				renderDone = 3;
		}
		if ( bufferEmptyI2C() && renderDone == 1 )
		{
			sendFramebufferStart();
			renderDone = 2;
		}

	#endif

	#ifndef EMULATION_IN_FIQ

		#ifndef USE_PWM_DIRECT
		static u32 nSamplesInThisRun = 0;
		#endif

		unsigned long long cycleCount = cycleCountC64;
		while ( cycleCount > nCyclesEmulated )
		{
		#ifndef USE_PWM_DIRECT
			static int start = 0;
			if ( nSamplesInThisRun > 2205 / 8 )
			{
				if ( !start )
				{
					m_pSound->Start();
					start = 1;
				} else
				{
					//pScheduler->MsSleep( 1 );
					pScheduler->Yield();
				}
				nSamplesInThisRun = 0;
			}
			nSamplesInThisRun++;
		#endif

			CACHE_PRELOADL2STRMW( &smpCur );

			static u32 carrySamples = 0;
			u32 samplesToEmulateX65536 = ( ( unsigned long long )65536 * ( unsigned long long )CLOCKFREQ ) / ( unsigned long long )SAMPLERATE + ( unsigned long long )carrySamples;

			u32 samplesToEmulate = samplesToEmulateX65536 >> 16;
			carrySamples = (samplesToEmulateX65536 & 65535);

			{
				#ifdef USE_PWM_DIRECT
				u32 cyclesToEmulate = samplesToEmulate;
				#else			
				u32 cyclesToEmulate = 2;
				#endif

				for ( u32 i = 0; i < NUM_SIDS; i++ )
					sid[ i ]->clock( cyclesToEmulate );

				outRegisters[ 27 ] = sid[ 0 ]->read( 27 );
				outRegisters[ 28 ] = sid[ 0 ]->read( 28 );

				nCyclesEmulated += cyclesToEmulate;

				// apply register updates (we do one-cycle emulation steps, but in case we need to catch up...)
				unsigned int readUpTo = ringWrite;

				if ( ringRead != readUpTo && nCyclesEmulated >= ringTime[ ringRead ] )
				{
					unsigned char A, D;

					u32 rv = ringBufGPIO[ ringRead ];
					D = rv & 255;
					A = (rv>>8)&31;
					u32 whichSID = rv >> 16;
	
					sid[ whichSID ]->write( A, D );

					ringRead++;
					ringRead &= ( RING_SIZE - 1 );
				}

			}

			//
			// mixer
			//

			CACHE_PRELOADL2STRMW( &sampleBuffer[ smpCur ] );
			s32 left = 0, right = 0;
			
			// yes, it's 1 byte shifted in the buffer, need to fix
			s32 l1, l2, l3, l4, r1, r2, r3, r4;
			l1 = sid[1]->output();
			l2 = sid[3]->output();
			l3 = sid[5]->output();
			l4 = sid[7]->output();
			r1 = sid[0]->output();
			r2 = sid[2]->output();
			r3 = sid[4]->output();
			r4 = sid[6]->output();

			left = ( l1 + l2 + l3 + l4 ) >> 1;
			right = ( r1 + r2 + r3 + r4 ) >> 1;

			right = max( -32767, min( 32767, right ) );
			left  = max( -32767, min( 32767, left ) );

			#ifdef USE_PWM_DIRECT
			putSample( left, right );
			#else
			putSample( left );
			putSample( right );
			#endif

		#if 1
			// vu meter
			static u32 vu_nValues = 0;
			static float vu_Sum[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
			
/*			if ( vu_Mode == 1 )
			{
				float t = (left+right) / (float)32768.0f * 0.4f;
				vu_Sum[ 0 ] += t * t * 1.25f;

				if ( ++ vu_nValues == 256*4 )
				{
					u32 i = 0;
					float vu_Volume = 50.0f * (log10( 1.0f + sqrt( (float)vu_Sum[ i ] / (float)vu_nValues ) ) );
					u32 v = vu_Volume * 1024.0f;
					vu_nLEDs = v >> 8;
					if ( vu_nLEDs > 4 ) vu_nLEDs = 4;
					vuMeter[ i ] = v >> 2;
					vuMeter[ i ] *= vuMeter[ i ];
					vuMeter[ i ] >>= 8;
					vu_Sum[ i ] = 0;
					vu_nValues = 0;
				}
			} else

			if ( vu_Mode == 2 )
			{
				vu_Sum[ 0 ] += (l1+r1) * (l1+r1) / (float)32768.0f / (float)32768.0f * 0.25f;
				vu_Sum[ 1 ] += (l2+r2) * (l2+r2) / (float)32768.0f / (float)32768.0f * 0.25f;
				vu_Sum[ 2 ] += (l3+r3) * (l3+r3) / (float)32768.0f / (float)32768.0f * 0.25f;
				vu_Sum[ 3 ] += (l4+r4) * (l4+r4) / (float)32768.0f / (float)32768.0f * 0.25f;

				if ( ++ vu_nValues == 256*4 )
				{
					for ( u32 i = 0; i < 4; i++ )
					{
						float vu_Volume = 50.0f * (log10( 1.0f + sqrt( (float)vu_Sum[ i ] / (float)vu_nValues ) ) );
						u32 v = vu_Volume * 1024.0f;
						vuMeter[ i ] = v >> 2;
						vuMeter[ i ] *= vuMeter[ i ];
						vuMeter[ i ] >>= 8;
						vu_Sum[ i ] = 0;
					}

					vu_nValues = 0;
				}
			}*/
			{
				float t = (left+right) / (float)32768.0f * 0.4f;
				vu_Sum[ 0 ] += t * t * 1.25f;

				if ( ++ vu_nValues == 256*4 )
				{
					u32 i = 0;
					{
						float vu_Volume = 50.0f * (log10( 1.0f + sqrt( (float)vu_Sum[ i ] / (float)vu_nValues ) ) );
						u32 v = vu_Volume * 1024.0f;
						if ( i == 0 )
						{
							vu_nLEDs = v >> 8;
							if ( vu_nLEDs > 4 ) vu_nLEDs = 4;
						}
						vu_Sum[ i ] = 0;
					}
					vu_nValues = 0;
				}
			}


			// ugly code which renders 3 oscilloscopes (SID1, SID2, FM) to HDMI and 1 for the OLED
			#include "oscilloscope_hack.h"
		#endif
		}
	#endif
	}

	m_InputPin.DisableInterrupt();
}


//static u32 releaseDMA = 0;

#ifdef COMPILE_MENU
void KernelSIDLaunchFIQHandler8( void *pParam )
{
	if ( !disableCart )
	{
		register u32 D;

		// after this call we have some time (until signals are valid, multiplexers have switched, the RPi can/should read again)
		START_AND_READ_ADDR0to7_RW_RESET_CS

		// update some counters
		UPDATE_COUNTERS_MIN( cycleCountC64, resetCounter )

		// read the rest of the signals
		WAIT_AND_READ_ADDR8to12_ROMLH_IO12_BA

		LAUNCH_FIQ( resetCounter )
	}
}

void KernelSIDFIQHandler8( void *pParam )
#else
void CKernel::FIQHandler (void *pParam)
#endif
{
	register u32 D;

	#ifdef COMPILE_MENU
	if ( launchPrg && !disableCart )
	{
		register u32 D;

		// after this call we have some time (until signals are valid, multiplexers have switched, the RPi can/should read again)
		START_AND_READ_ADDR0to7_RW_RESET_CS

		// update some counters
		UPDATE_COUNTERS_MIN( cycleCountC64, resetCounter )

		// read the rest of the signals
		WAIT_AND_READ_ADDR8to12_ROMLH_IO12_BA

		LAUNCH_FIQ( resetCounter )
	}
	#endif

	static s32 latchDelayOut = 10;

	START_AND_READ_ADDR0to7_RW_RESET_CS

	if ( CPU_RESET ) {
		resetReleased = 0; resetPressed = 1; resetCounter ++;
	} else {
		if ( resetPressed )	resetReleased = 1;
		resetPressed = 0;
	}

	static u32 fCount = 0;
	fCount ++;
	fCount &= 255;

	cycleCountC64 ++;

	#ifdef COMPILE_MENU
	// preload cache
	if ( !( launchPrg && !disableCart ) )
	{
		CACHE_PRELOADL1STRMW( &ringWrite );
		CACHE_PRELOADL1STRM( &sampleBuffer[ smpLast ] );
		CACHE_PRELOADL1STRM( &outRegisters[ 0 ] );
		CACHE_PRELOADL1STRM( &outRegisters[ 16 ] );
	}
	#endif

	WAIT_AND_READ_ADDR8to12_ROMLH_IO12_BA

	#ifdef COMPILE_MENU
	if ( resetCounter > 3  )
	{
		disableCart = transferStarted = 0;
		SETCLR_GPIO( configGAMEEXROMSet | bNMI, configGAMEEXROMClr );
		FINISH_BUS_HANDLING
		return;
	}
	#endif


	//  __   ___       __      __     __  
	// |__) |__   /\  |  \    /__` | |  \ 
	// |  \ |___ /~~\ |__/    .__/ | |__/ 
	//
	if ( cfgRegisterRead && CPU_READS_FROM_BUS && SID_ACCESS )
	{
		u32 A = ( g2 >> A0 ) & 31;
		u32 D = outRegisters[ A ];

		WRITE_D0to7_TO_BUS( D )

		FINISH_BUS_HANDLING
		return;
	} else
	//       __    ___  ___     __     __  
	// |  | |__) |  |  |__     /__` | |  \ 
	// |/\| |  \ |  |  |___    .__/ | |__/ 
	//                                   
	if ( CPU_WRITES_TO_BUS && SID_ACCESS ) 
	{
		READ_D0to7_FROM_BUS( D )

		register u32 A = GET_ADDRESS0to7 | ((GET_ADDRESS8to12&1)<<8);
		register u32 whichSID = ((A>>6)&6) | ((A>>5)&1);
		A &= 31;
		
		ringBufGPIO[ ringWrite ] = D | (A << 8) | (whichSID << 16);

		//ringBufGPIO[ ringWrite ] = ( remapAddr | ( D << D0 ) ) & ~bIO2;
		ringTime[ ringWrite ] = cycleCountC64;
		ringWrite ++;
		ringWrite &= ( RING_SIZE - 1 );
		CACHE_PRELOADL1STRMW( &ringBufGPIO[ ringWrite ] );

		// optionally we could directly set the SID-output registers (instead of where the emulation runs)
		//u32 A = ( g2 >> A0 ) & 31;
		//outRegisters[ A ] = g1 & D_FLAG;

		FINISH_BUS_HANDLING
		return;
	}

	//  __                 __       ___  __       ___ 
	// |__) |  |  |\/|    /  \ |  |  |  |__) |  |  |  
	// |    |/\|  |  |    \__/ \__/  |  |    \__/  |  
	// OPTIONAL
	//											
	#ifdef USE_PWM_DIRECT
	static unsigned long long samplesElapsedBeforeFIQ = 0;

	unsigned long long samplesElapsedFIQ = ( ( unsigned long long )cycleCountC64 * ( unsigned long long )SAMPLERATE ) / ( unsigned long long )CLOCKFREQ;

	if ( samplesElapsedFIQ != samplesElapsedBeforeFIQ )
	{
		write32( ARM_GPIO_GPCLR0, bCTRL257 ); 
		samplesElapsedBeforeFIQ = samplesElapsedFIQ;

		u32 s = getSample();
		u16 s1 = s & 65535;
		u16 s2 = s >> 16;

		s32 d1 = (s32)( ( *(s16*)&s1 + 32768 ) * PWMRange ) >> 17;
		s32 d2 = (s32)( ( *(s16*)&s2 + 32768 ) * PWMRange ) >> 17;
		write32( ARM_PWM_DAT1, d1 );
		write32( ARM_PWM_DAT2, d2 );
		RESET_CPU_CYCLE_COUNTER
		return;
	} 
	#endif
	

	//           ___  __       
	// |     /\   |  /  ` |__| 
	// |___ /~~\  |  \__, |  | 
	//
	#ifdef USE_LATCH_OUTPUT
	if ( --latchDelayOut == 1 && renderDone == 3 )
	{
		prefetchI2C();
	}
	if ( latchDelayOut <= 0 && renderDone == 3 )
	{
		latchDelayOut = 2;
		prepareOutputLatch();
		if ( bufferEmptyI2C() ) renderDone = 0;
		OUTPUT_LATCH_AND_FINISH_BUS_HANDLING
		return;

	}
	#endif

	static u32 lastButtonPressed = 0;

	if ( lastButtonPressed > 0 )
		lastButtonPressed --;

	if ( BUTTON_PRESSED && lastButtonPressed == 0 )
	{
		vu_Mode = ( vu_Mode + 1 ) & 3;
		lastButtonPressed = 100000;
	}

#if 1
	{
		setLatchFIQ( LATCH_ON[ vu_nLEDs ] );
		clrLatchFIQ( LATCH_OFF[ vu_nLEDs ] );
	}
#endif

	FINISH_BUS_HANDLING
}

#ifndef COMPILE_MENU
int main( void )
{
	CKernel kernel;
	if ( kernel.Initialize() )
		kernel.Run();

	halt();
	return EXIT_HALT;
}
#endif