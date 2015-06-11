#include "control.h"
#include "ets_sys.h"
#include "os_type.h"
#include "mem.h"
#include "spi_flash.h"
#include "lsm9ds1.h"
#include "bmp085.h"
#include "osapi.h"
#include "mem.h"
#include "c_types.h"
#include "user_interface.h"
#include "ets_sys.h"
#include "driver/uart.h"
#include "espconn.h"
#include "http.h"
#include "flash_rewriter.h"

extern SpiFlashChip * flashchip;
struct espconn *pespconn;

//GYR X, GYR Y, GYR Z
//ACC X, ACC Y, ACC Z
//MAG X, MAG Y, MAG Z
//MTEMP, BTEMP, BAROMETER

uint32_t pressure;
uint16_t bmptemp;
uint32_t systemFrame;

int in_range_setting;
int stream_data = 0;
int stream_data_raw = 0;

int32_t  gyrNow[3];
int32_t	 accNow[3];
int32_t  magNow[3];
int32_t  barNow;
int32_t  tempNow;  //tenths of degrees 
int16_t  targetAxes[4]; //Expected -1000..1000 (L/R, FWD/BKD, Orient, elevator)

int		  voltNow;
uint8_t  gMotors[4];
uint8_t  motors_automatic = 0; //whether in flight? Maybe?

//Used only for centering.
int32_t  gyrIIR[3];
int32_t  accIIR[3];
int32_t  magIIR[3];
int32_t  barIIR;


uint32_t calgyro[3];
uint32_t calacc[3];
uint32_t calmag[3];

int16_t sensordata[10];
int8_t  sensorstatus;

int bus_online = 1;

struct SaveSector settings;

void ICACHE_FLASH_ATTR ResetIIR()
{
	int i;
	for( i = 0; i < 3; i++ )
		gyrIIR[i] = gyrNow[i]<<GYRIIRDEP;
	for( i = 0; i < 3; i++ )
		accIIR[i] = accNow[i]<<ACCIIRDEP;
	for( i = 0; i < 3; i++ )
		magIIR[i] = magNow[i]<<MAGIIRDEP;
	barIIR = barNow<<BARIIRDEP;
}

void ICACHE_FLASH_ATTR StartRange()
{
	int i;
	ResetIIR();
	for( i = 2; i < 3; i++ )
	{
		settings.accelmin[i] = accIIR[i]-1;
		settings.accelmax[i] = accIIR[i]+1;
	}
	for( i = 0; i < 3; i++ )
	{
		settings.magmin[i] = magIIR[i]-1;
		settings.magmax[i] = magIIR[i]+1;
	}
	
}

void ICACHE_FLASH_ATTR DoZero()
{
	int i;
	for( i = 0; i < 3; i++ )
		settings.gyrocenter[i] = gyrIIR[i];
	for( i = 0; i < 2; i++ )
	{
		settings.acccenter[i] = accIIR[i];
	}
}



//Scanning doesn't seem to work in SoftAP mode, so, this may not be that useful.

int need_to_switch_back_to_soft_ap = 0; //0 = no, 1 = will need to. 2 = do it now.
#define MAX_STATIONS 20
struct totalscan_t
{
	char name[32];
	char mac[18]; //string
	int8_t rssi;
	uint8_t channel;
	uint8_t encryption;
} totalscan[MAX_STATIONS];
int scanplace = 0;



static void ICACHE_FLASH_ATTR scandone(void *arg, STATUS status)
{
	scaninfo *c = arg; 
	struct bss_info *inf; 

	if( need_to_switch_back_to_soft_ap == 1 )
		need_to_switch_back_to_soft_ap = 2;

	printf("!%p\n",c->pbss);

	if (!c->pbss) {
		scanplace = -1;
		return;
	}
//		printf("!%s\n",inf->ssid);
	STAILQ_FOREACH(inf, c->pbss, next) {
		printf( "%s\n", inf->ssid );
		ets_memcpy( totalscan[scanplace].name, inf->ssid, 32 );
		ets_sprintf( totalscan[scanplace].mac, MACSTR, MAC2STR( inf->bssid ) );
		//ets_memcpy( totalscan[scanplace].mac, "not implemented", 16 );
		totalscan[scanplace].rssi = inf->rssi;
		totalscan[scanplace].channel = inf->channel;
		totalscan[scanplace].encryption = inf->authmode;
		inf = (struct bss_info *) &inf->next;
		scanplace++;
		if( scanplace == MAX_STATIONS ) break;
	}
	printf( "OK\n" );

}



//Timer event.
void ICACHE_FLASH_ATTR controltimer()
{
	int i;
	char buffer[128];
	char * buffend;
	buffend = buffer;
	static int agmfailures = 0;

	if( !bus_online ) return;

	sensorstatus = ReadAGM( sensordata );

	if( sensorstatus < -10 )
	{
		agmfailures++;
		if( agmfailures > 10 )
		{
			agmfailures = 0;
			SetupLSM();
		}
	}
	else
	{
		agmfailures = 0;
	}

	if( ( systemFrame & 0x07 ) == 0 )
		pressure = GetBMP085();
	else if( ( systemFrame & 0x07 ) == 2 )
		bmptemp = GetBMPTemp();
	else if( ( systemFrame & 0x07 ) == 4 )
	{
		DoBMPCalcs();

		tempNow = bmp_degc;
		barNow = bmp_pa;

		barIIR = (barIIR - (barIIR>>BARIIRDEP)) + barNow;
	}


	for( i = 0; i < 3; i++ )
	{
		gyrNow[i] = sensordata[i+3];
		gyrIIR[i] = ( gyrIIR[i] - ( gyrIIR[i] >> GYRIIRDEP ) ) + gyrNow[i];

		calgyro[i] = ( (int32_t)gyrNow[i] - ((int32_t)settings.gyrocenter[i]>>GYRIIRDEP) );
	}

	for( i = 2; i < 3; i++ )
	{
		accNow[i] = sensordata[i];
		accIIR[i] = ( accIIR[i] - ( accIIR[i] >> ACCIIRDEP ) ) + (int32_t)accNow[i];

		int32_t calarea = ((int32_t)settings.accelmax[i] - (int32_t)settings.accelmin[i]) >> (ACCIIRDEP+1);
		if( calarea == 0 ) calarea = 1;
		calacc[i] = 1000 * ( ((int32_t)accNow[i]) - ((int32_t)settings.accelmin[i]>>ACCIIRDEP) ) / calarea - 1000;
	}

	for( i = 0; i < 2; i++ )
	{
		accNow[i] = sensordata[i];
		accIIR[i] = ( accIIR[i] - ( accIIR[i] >> ACCIIRDEP ) ) + accNow[i];

		calacc[i] = (((int32_t)accNow[i]) - ((int32_t)settings.acccenter[i]>>ACCIIRDEP));
	}


	for( i = 0; i < 3; i++ )
	{
		magNow[i] = sensordata[i+6];
		magIIR[i] = ( magIIR[i] - ( magIIR[i] >> MAGIIRDEP ) ) + (int32_t)magNow[i];
		int32_t magarea = ((int32_t)settings.magmax[i] - (int32_t)settings.magmin[i]) >> (MAGIIRDEP+1);
		if( magarea == 0 ) magarea = 1;
		calmag[i] = 1000 * ( ((int32_t)magNow[i]) - ((int32_t)settings.magmin[i]>>MAGIIRDEP) ) / magarea - 1000;
	}


	//Hacky (For now)
//	gMotors[0] = gMotors[1] = gMotors[2] = gMotors[3] = 0x80;

	//Closed loop control.
	if( motors_automatic )
	{
		int32_t spin = ((int32_t)(calgyro[2]));  //POSITIVE = moving counter-clockwise.
		int32_t leftright = ((int32_t)(calacc[0])); //LEFT = POSITIVE, RIGHT = NEGATIVE
		int32_t fwdbak = ((int32_t)(calacc[1]));    //FWD = NEGATIVE, REVERSE = POSITIVE

		//Joystick foward = negative
		//Joystick right = positive.

		leftright += targetAxes[0]; //Positve = move right
		fwdbak -= targetAxes[1];   //Positive = 
		spin -= targetAxes[2];

		//M0: Front, right, clockwise
		//M1: Rear, right, counter-clockwise
		//M2: Rear, left, clockwise
		//M3: Front, left, counter-clockwise

		int m1 = -leftright - fwdbak - spin;
		int m2 =  leftright - fwdbak + spin;
		int m3 =  leftright + fwdbak - spin;
		int m4 = -leftright + fwdbak + spin;

		ets_sprintf( buffer, "MD\t%d\t%d\t%d\t%d\n", m1, m2, m3, m4 );
		if( pespconn )
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );

		m1 = (m1 + (targetAxes[3]))>>6;
		m2 = (m2 + (targetAxes[3]))>>6;
		m3 = (m3 + (targetAxes[3]))>>6;
		m4 = (m4 + (targetAxes[3]))>>6;

		if( m1 < 0 ) m1 = 0;
		if( m2 < 0 ) m2 = 0;
		if( m3 < 0 ) m3 = 0;
		if( m4 < 0 ) m4 = 0;

		if( m1 > 200 ) m1 = 200;
		if( m2 > 200 ) m2 = 200;
		if( m3 > 200 ) m3 = 200;
		if( m4 > 200 ) m4 = 200;

		gMotors[0] = m1;
		gMotors[1] = m2;
		gMotors[2] = m3;
		gMotors[3] = m4;



	}
	else
	{
		for( i = 0; i < 4; i++ )
		{
			if( gMotors[i] > 0 ) gMotors[i]--;
		}
	}



	voltNow = RunAVRTool( gMotors );



	if( in_range_setting )
	{
		for( i = 2; i < 3; i++ )
		{
			if( accIIR[i] > settings.accelmax[i] )
				settings.accelmax[i] = accIIR[i];

			if( accIIR[i] < settings.accelmin[i] )
				settings.accelmin[i] = accIIR[i];
		}
		for( i = 0; i < 3; i++ )
		{
			if( magIIR[i] > settings.magmax[i] )
				settings.magmax[i] = magIIR[i];

			if( magIIR[i] < settings.magmin[i] )
				settings.magmin[i] = magIIR[i];
		}
	}


	//Before here, we should do any further calibration needed.

	//Past here, we can use:
	//	float calgyro[3];
	//	float calacc[3];
	//	float calmag[3];
	//  barIIR, barNow

	if( stream_data )
	{
		buffend += FillCal( buffend );

		if( pespconn )
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
	}


	if( stream_data_raw )
	{
		buffend += FillRaw( buffend );

		if( pespconn )
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
	}

	systemFrame++;



	if(	need_to_switch_back_to_soft_ap == 2 )
	{
		need_to_switch_back_to_soft_ap = 0;
		wifi_set_opmode_current( SOFTAP_MODE );
	}

	uart_tx_one_char( 0 );

}


//Called when new packet comes in.
void ICACHE_FLASH_ATTR issue_command(void *arg, char *pusrdata, unsigned short len)
{
	pespconn = (struct espconn *)arg;
	//We accept commands on this.

	switch( pusrdata[0] )
	{
	case 'd': case 'D':
		((void(*)())0x40000080)();
		break;
	case 'b': case 'B':
		if( pusrdata[1] == '0' )
		{
			espconn_sent( pespconn, "B0\r\n", 4 );
			bus_online = 0;
		}
		else if( pusrdata[1] == '1' )
		{
			espconn_sent( pespconn, "B1\r\n", 4 );
			bus_online = 1;
		}
		else
		{
			//No.
		}
		break;

	case 't': case 'T': //Stream Data (T0, T1)
		if( pusrdata[1] == '0' )
		{
			espconn_sent( pespconn, "T0\r\n", 4 );
			stream_data = 0;
		}
		else if( pusrdata[1] == '1' )
		{
			espconn_sent( pespconn, "T1\r\n", 4 );
			stream_data = 1;
		}
		else
		{
			//No.
		}
		break;
	case 'u': case 'U': //Stream Data, RAW (U0, U1)
		if( pusrdata[1] == '0' )
		{
			espconn_sent( pespconn, "U0\r\n", 4 );
			stream_data_raw = 0;
		}
		else if( pusrdata[1] == '1' )
		{
			espconn_sent( pespconn, "U1\r\n", 4 );
			stream_data_raw = 1;
		}
		else
		{
			//No.
		}
		break;
	case 'z': case 'Z': //Zero ESP (Z)
		DoZero();
		espconn_sent( pespconn, "Z\r\n", 4 );
		break;
	case 'j': case 'J': //Joystick input (Bank Right, Bank Forward, Turn, Elevator)
	{
		const char * n1 = (char*)&pusrdata[1];
		const char * n2 = (const char *) ets_strstr( n1, ":" );
		const char * n3 = (const char *) ets_strstr( n2, ":" );
		const char * n4 = (const char *) ets_strstr( n3, ":" );

		if( !n1 || !n2 || !n3 || !n4 )
		{
			espconn_sent( pespconn, "!J\r\n", 4 );
		}

		targetAxes[0] = my_atoi(n1);
		targetAxes[1] = my_atoi(n2);
		targetAxes[2] = my_atoi(n3);
		targetAxes[3] = my_atoi(n4);

		espconn_sent( pespconn, "J\r\n", 3 );
		break;
	}
	case 'r': case 'R': //Start/Stop Range Setting (R1, R0) When in range setting, rotate ESP slowly.
		if( pusrdata[1] == '1' )
		{
			in_range_setting = 1;
			StartRange();
			espconn_sent( pespconn, "R1\r\n", 4 );
		}
		else if( pusrdata[1] == '0' )
		{
			in_range_setting = 0;
			espconn_sent( pespconn, "R0\r\n", 4 );
		}
		break;
	case 'p': case 'P': //Print calibration data (P)
		{
			char buffer[256];
			char * buffend;
			buffend = buffer;
			buffend += ets_sprintf(buffend, "P6\r\n#GC\t%d\t%d\t%d\r\n",((int32_t)(settings.gyrocenter[0])), ((int32_t)(settings.gyrocenter[1])), ((int32_t)(settings.gyrocenter[2])) );
			buffend += ets_sprintf(buffend, "#AC\t%d\t%d\t%d\r\n",((int32_t)(settings.acccenter[0])), ((int32_t)(settings.acccenter[1])), ((int32_t)(settings.acccenter[2])) );
			buffend += ets_sprintf(buffend, "#AL\t%d\t%d\t%d\r\n",((int32_t)(settings.accelmin[0])), ((int32_t)(settings.accelmin[1])), ((int32_t)(settings.accelmin[2])) );
			buffend += ets_sprintf(buffend, "#AH\t%d\t%d\t%d\r\n",((int32_t)(settings.accelmax[0])), ((int32_t)(settings.accelmax[1])), ((int32_t)(settings.accelmax[2])) );
			buffend += ets_sprintf(buffend, "#ML\t%d\t%d\t%d\r\n",((int32_t)(settings.magmin[0])), ((int32_t)(settings.magmin[1])), ((int32_t)(settings.magmin[2])) );
			buffend += ets_sprintf(buffend, "#MH\t%d\t%d\t%d\r\n",((int32_t)(settings.magmax[0])), ((int32_t)(settings.magmax[1])), ((int32_t)(settings.magmax[2])) );
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
			break;
		}
	case 's': case 'S': //Save Calibration Data (S)
		SaveSettings();
		espconn_sent( pespconn, "S\r\n", 3 );
		break;

	case 'M': case 'm':
	{
		if( len > 1 )
		switch( pusrdata[1] )
		{
		case '?':
		{
			char buffer[256];
			char * buffend;
			buffend = buffer;

			buffend += ets_sprintf(buffend, "M?%d:%d:%d:%d:%d\r\n", motors_automatic, gMotors[0], gMotors[1], gMotors[2], gMotors[3] );
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
			break;
		}
		case 'A': case 'a':
			motors_automatic = 1;
			espconn_sent( pespconn, "MA\r\n", 4 );
			break;
		case 'M': case 'm':
			espconn_sent( pespconn, "MM\r\n", 4 );
			motors_automatic = 0;
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		{
			const char * colon = (const char *) ets_strstr( (char*)&pusrdata[2], ":" );
			if( colon )
			{
				int mtr = pusrdata[1]-'0';
				int val = my_atoi( colon+1 );
				char sto[128];
				int sl = ets_sprintf( sto, "M%d:%d\r\n", mtr, val );
				espconn_sent( pespconn, sto, sl );
				gMotors[mtr] = val;
			}
			break;
		}
		}
		break;
	}

	case 'f': case 'F':  //Flashing commands (F_)
	{
		flashchip->chip_size = 0x01000000;
//		ets_printf( "PC%p\n", &pusrdata[2] );
		const char * colon = (const char *) ets_strstr( (char*)&pusrdata[2], ":" );
		int nr = my_atoi( &pusrdata[2] );

		switch (pusrdata[1])
		{
		case 'e': case 'E':  //(FE#\n) <- # = sector.
		{
			char  __attribute__ ((aligned (16))) buffer[50];
			char * buffend;
			buffend = buffer;
			spi_flash_erase_sector( nr );	//SPI_FLASH_SEC_SIZE      4096
			buffend += ets_sprintf(buffend, "FE%d\r\n", nr );
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
			break;
		}

		case 'b': case 'B':  //(FE#\n) <- # = sector.
		{
			char  __attribute__ ((aligned (16))) buffer[50];
			char * buffend;
			buffend = buffer;
			//spi_flash_erase_sector( nr );	//SPI_FLASH_SEC_SIZE      4096

			SPIEraseBlock( nr );

			buffend += ets_sprintf(buffend, "FB%d\r\n", nr );
			espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
			break;
		}

		case 'm': case 'M': //Execute the flash re-writer
			{
				char ret[128];
				int n = ets_sprintf( ret, "FM" );
				espconn_sent( pespconn, ret, n );
				int r = (*GlobalRewriteFlash)( &pusrdata[2], len-2 );
				n = ets_sprintf( ret, "!FM%d", r );
				espconn_sent( pespconn, ret, n );
			}
		case 'w': case 'W':	//Flash Write (FW#\n) <- # = byte pos.
			if( colon )
			{
				char  __attribute__ ((aligned (32))) buffer[1300];
				char * buffend;
				buffend = buffer;
				buffer[0] = 0;
				colon++;
				const char * colon2 = (const char *) ets_strstr( (char*)colon, ":" );
				if( colon2 )
				{
					colon2++;
					int datlen = (int)len - (colon2 - pusrdata);
					ets_memcpy( buffer, colon2, datlen );
					spi_flash_write( nr, (uint32*)buffer, (datlen/4)*4 );

					//SPIWrite( nr, (uint32_t*)buffer, (datlen/4)*4 );

					buffend = buffer;
					buffend += ets_sprintf(buffend, "FW%d\r\n", nr );
					if( ets_strlen( buffer ) )
						espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
				}
			}
			break;
		case 'r': case 'R':	//Flash Read (FR#\n) <- # = sector.
			if( colon )
			{
				char  __attribute__ ((aligned (16))) buffer[1300];
				char * buffend;
				buffend = buffer;
				buffer[0] = 0;
				colon++;
				int datlen = my_atoi( colon );
				if( datlen <= 1280 )
				{
					buffend += ets_sprintf(buffend, "FR%08d:%04d:", nr, datlen );
					int frontlen = buffend - buffer;
					spi_flash_read( nr, (uint32*)buffend, (datlen/4)*4 );
					espconn_sent( pespconn, buffer, frontlen + datlen );
					buffer[0] = 0;
					if( ets_strlen( buffer ) )
						espconn_sent( pespconn, buffer, ets_strlen( buffer ) );
				}
			}
			break;
		}

		flashchip->chip_size = 0x00080000;
		break;
	}
	case 'w': case 'W':	// (W1:SSID:PASSWORD) (To connect) or (W2) to be own base station.  ...or WI, to get info... or WS to scan.
	{
		char * colon = (char *) ets_strstr( (char*)&pusrdata[2], ":" );
		char * colon2 = (colon)?((char *)ets_strstr( (char*)(colon+1), ":" )):0;
		char * extra = colon2;
		char  __attribute__ ((aligned (16))) buffer[1300];
		char * buffend;
		buffend = buffer;
		buffer[0] = 0;

		if( extra )
		{
			for( ; *extra; extra++ )
			{
				if( *extra < 32 )
				{
					*extra = 0;
					break;
				}
			}
		}

		switch (pusrdata[1])
		{
		case '1': //Station mode
			if( colon && colon2 )
			{
				int c1l = 0;
				int c2l = 0;
				struct station_config stationConf;
				*colon = 0;  colon++;
				*colon2 = 0; colon2++;
				c1l = ets_strlen( colon );
				c2l = ets_strlen( colon2 );
				if( c1l > 32 ) c1l = 32;
				if( c2l > 32 ) c2l = 32;
				os_bzero( &stationConf, sizeof( stationConf ) );

				printf( "Switching to: \"%s\"/\"%s\".\n", colon, colon2 );

				os_memcpy(&stationConf.ssid, colon, c1l);
				os_memcpy(&stationConf.password, colon2, c2l);

				wifi_set_opmode( 1 );
				wifi_station_set_config(&stationConf);
				wifi_station_connect();
				espconn_sent( pespconn, "W1\r\n", 4 );

				printf( "Switching.\n" );
			}
			break;
		case '2':
			{
				wifi_set_opmode_current( 1 );
				wifi_set_opmode( 2 );
				espconn_sent( pespconn, "W2\r\n", 4 );
			}
			break;
		case 'I':
			{
				struct station_config sc;
				int mode = wifi_get_opmode();
				char buffer[200];
				char * buffend = &buffer[0];

				buffend += ets_sprintf( buffend, "WI%d", mode );

				wifi_station_get_config( &sc );

				buffend += ets_sprintf( buffend, ":%s:%s", sc.ssid, sc.password );

				espconn_sent( pespconn, buffer, buffend - buffer );
			}
			break;
		case 'S': case 's':
			{
				char buffer[1024];
				char * buffend = &buffer[0];
				int i, r;
				printf( "A\n" );
				if( wifi_get_opmode() == SOFTAP_MODE )
				{
					wifi_set_opmode_current( STATION_MODE );
					need_to_switch_back_to_soft_ap = 1;
					r = wifi_station_scan(0, scandone );
				}
				else
				{
					r = wifi_station_scan(0, scandone );
				}
				printf( "B\n" );

				buffend += ets_sprintf( buffend, "WS%d\n", r );
				uart0_sendStr(buffer);

				espconn_sent( pespconn, buffer, buffend - buffer );

			}
			break;
		case 'R': case 'r':
			{
				char buffer[1024];
				char * buffend = &buffer[0];
				int i, r;

				buffend += ets_sprintf( buffend, "WR%d\n", scanplace );
				
				for( i = 0; i < scanplace; i++ )
				{
					buffend += ets_sprintf( buffend, "#%s\t%s\t%d\t%d\t%s\n", 
						totalscan[i].name, totalscan[i].mac, totalscan[i].rssi, totalscan[i].channel, enctypes[totalscan[i].encryption] );
				}

				espconn_sent( pespconn, buffer, buffend - buffer );

			}
			break;
		}
		break;
	}
	case '?':
		espconn_sent( pespconn, "No help. Read source\r\n", 22 );
		break;
	}


}




void ICACHE_FLASH_ATTR ControlInit()
{
	flashchip->chip_size = 0x01000000;
	spi_flash_read( 0x3D000, (uint32*)&settings, sizeof( settings ) );
	flashchip->chip_size = 0x00080000;
}

void ICACHE_FLASH_ATTR SaveSettings()
{
	flashchip->chip_size = 0x01000000;
	spi_flash_erase_sector( 0x3D000/4096 );
	spi_flash_write( 0x3D000, (uint32*)&settings, sizeof( settings ) );
	flashchip->chip_size = 0x00080000;
}


int ICACHE_FLASH_ATTR FillRaw( char * buffend )
{
	int i;
	char * buffstart = buffend;

	buffend += ets_sprintf( buffend, "UD\t" );

	buffend += ets_sprintf( buffend, "%9d\t", systemFrame );

	for( i = 0; i < 3; i++ )
	{
		buffend += ets_sprintf( buffend, "%6d\t", ((int32_t)(gyrNow[i])) );
	}

	for( i = 0; i < 3; i++ )
	{
		buffend += ets_sprintf( buffend, "%6d\t", ((int32_t)(accNow[i])) );
	}

	for( i = 0; i < 3; i++ )
	{
		buffend += ets_sprintf( buffend, "%6d\t", ((int32_t)(magNow[i])) );
	}

	buffend += ets_sprintf( buffend, "%6d\t%3d\t%3d\t%d\r\n", barNow, tempNow, voltNow, sensorstatus );

	return buffend - buffstart;
}

int ICACHE_FLASH_ATTR FillCal( char * buffend )
{
	int i;
	char * buffstart = buffend;

	buffend += ets_sprintf( buffend, "TD\t" );

	buffend += ets_sprintf( buffend, "%9d\t", systemFrame );

	for( i = 0; i < 3; i++ )
	{
		buffend += ets_sprintf( buffend, "%6d\t", calgyro[i] );
	}

	for( i = 0; i < 3; i++ )
	{
		buffend += ets_sprintf( buffend, "%6d\t", calacc[i] );
	}

	for( i = 0; i < 3; i++ )
	{
		buffend += ets_sprintf( buffend, "%6d\t", calmag[i] );
	}

	buffend += ets_sprintf( buffend, "%6d\t%3d\t%3d\t%d\r\n", barIIR>>BARIIRDEP, tempNow, voltNow, sensorstatus );
	return buffend - buffstart;
}

