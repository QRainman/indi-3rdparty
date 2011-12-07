#if 0
    QSI CCD
    INDI Interface for Quantum Scientific Imaging CCDs
    Based on FLI Indi Driver by Jasem Mutlaq.
    Copyright (C) 2009 Sami Lehti (sami.lehti@helsinki.fi)

    (2011-12-10) Updated by Jasem Mutlaq:
        + Fixed compiler warnings.
        + Reduced message traffic.
        + Added filter name property.
        + Rewrote driver to be based on INDI::CCD
        + Added snooping on telescopes.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <zlib.h>

#include <fitsio.h>

#include "qsiapi.h"
#include "QSIError.h"
#include "indidevapi.h"
#include "eventloop.h"
#include "indicom.h"
#include "qsi_ccd.h"

void ISInit(void);
void ISPoll(void *);

double min(void);
double max(void);

#define EXPOSE_TAB	"Expose"

#define MAX_CCD_TEMP	45		/* Max CCD temperature */
#define MIN_CCD_TEMP	-55		/* Min CCD temperature */
#define MAX_X_BIN	16		/* Max Horizontal binning */
#define MAX_Y_BIN	16		/* Max Vertical binning */
#define MAX_PIXELS	4096		/* Max number of pixels in one dimension */
#define POLLMS		1000		/* Polling time (ms) */
#define TEMP_THRESHOLD  .25		/* Differential temperature threshold (C)*/
#define NFLUSHES	1		/* Number of times a CCD array is flushed before an exposure */

#define LAST_FILTER  4          /* Max slot index */
#define FIRST_FILTER 0          /* Min slot index */

#define currentFilter   FilterN[0].value

std::auto_ptr<QSICCD> qsiCCD(0);

 void ISInit()
 {
    static int isInit =0;

    if (isInit == 1)
        return;

     isInit = 1;
     if(qsiCCD.get() == 0) qsiCCD.reset(new QSICCD());
     //IEAddTimer(POLLMS, ISPoll, NULL);

 }

 void ISGetProperties(const char *dev)
 {
         ISInit();
         qsiCCD->ISGetProperties(dev);
 }

 void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int num)
 {
         ISInit();
         qsiCCD->ISNewSwitch(dev, name, states, names, num);
 }

 void ISNewText(	const char *dev, const char *name, char *texts[], char *names[], int num)
 {
         ISInit();
         qsiCCD->ISNewText(dev, name, texts, names, num);
 }

 void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int num)
 {
         ISInit();
         qsiCCD->ISNewNumber(dev, name, values, names, num);
 }

 void ISNewBLOB (const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[], int n)
 {
   INDI_UNUSED(dev);
   INDI_UNUSED(name);
   INDI_UNUSED(sizes);
   INDI_UNUSED(blobsizes);
   INDI_UNUSED(blobs);
   INDI_UNUSED(formats);
   INDI_UNUSED(names);
   INDI_UNUSED(n);
 }
 void ISSnoopDevice (XMLEle *root)
 {
     ISInit();
     qsiCCD->ISSnoopDevice(root);
 }


 QSICCD::QSICCD()
 {
     targetFilter = 0;

     ResetSP = new ISwitchVectorProperty;
     CoolerSP = new ISwitchVectorProperty;
     ShutterSP = new ISwitchVectorProperty;
     CoolerNP = new INumberVectorProperty;

     TemperatureRequestNP = new INumberVectorProperty;
     TemperatureNP = new INumberVectorProperty;


 }


QSICCD::~QSICCD()
{
    //dtor
    delete RawFrame;
    RawFrame = NULL;
}

const char * QSICCD::getDefaultName()
{
        return (char *)"QSI CCD";
}

bool QSICCD::initProperties()
{
    // Init parent properties first
    INDI::CCD::initProperties();

    IUFillSwitch(&ResetS[0], "RESET", "Reset", ISS_OFF);
    IUFillSwitchVector(ResetSP, ResetS, 1, deviceName(), "FRAME_RESET", "Frame Values", IMAGE_SETTINGS_TAB, IP_WO, ISR_1OFMANY, 0, IPS_IDLE);

    IUFillSwitch(&CoolerS[0], "CONNECT_COOLER", "ON", ISS_OFF);
    IUFillSwitch(&CoolerS[1], "DISCONNECT_COOLER", "OFF", ISS_OFF);
    IUFillSwitchVector(CoolerSP, CoolerS, 2, deviceName(), "COOLER_CONNECTION", "Cooler", MAIN_CONTROL_TAB, IP_WO, ISR_1OFMANY, 0, IPS_IDLE);

    IUFillSwitch(&ShutterS[0], "SHUTTER_ON", "Manual open", ISS_OFF);
    IUFillSwitch(&ShutterS[1], "SHUTTER_OFF", "Manual close", ISS_OFF);
    IUFillSwitchVector(ShutterSP, ShutterS, 2, deviceName(), "SHUTTER_CONNECTION", "Shutter", MAIN_CONTROL_TAB, IP_WO, ISR_1OFMANY, 0, IPS_IDLE);

    IUFillNumber(&CoolerN[0], "CCD_COOLER_VALUE", "Cooling Power (%)", "%+06.2f", 0., 1., .2, 0.0);
    IUFillNumberVector(CoolerNP, CoolerN, 1, deviceName(), "CCD_COOLER", "Cooling Power", MAIN_CONTROL_TAB, IP_RO, 60, IPS_IDLE);

    IUFillNumber(&TemperatureRequestN[0], "CCD_TEMPERATURE_VALUE", "Temperature (C)", "%5.2f", MIN_CCD_TEMP, MAX_CCD_TEMP, 0., 0.);
    IUFillNumberVector(TemperatureRequestNP, TemperatureN, 1, deviceName(), "CCD_TEMPERATURE_REQUEST", "Temperature", MAIN_CONTROL_TAB, IP_WO, 60, IPS_IDLE);

    IUFillNumber(&TemperatureN[0], "CCD_TEMPERATURE_VALUE", "Temperature (C)", "%5.2f", MIN_CCD_TEMP, MAX_CCD_TEMP, 0., 0.);
    IUFillNumberVector(TemperatureNP, TemperatureN, 1, deviceName(), "CCD_TEMPERATURE", "Temperature", MAIN_CONTROL_TAB, IP_RO, 60, IPS_IDLE);

    addDebugControl();

}

bool QSICCD::updateProperties()
{
    INDI::CCD::updateProperties();

    if (isConnected())
    {
        defineNumber(TemperatureRequestNP);
        defineNumber(TemperatureNP);
        defineSwitch(ResetSP);
        defineSwitch(CoolerSP);
        defineSwitch(ShutterSP);
        defineNumber(CoolerNP);
    }
    else
    {
        deleteProperty(TemperatureRequestNP->name);
        deleteProperty(TemperatureNP->name);
        deleteProperty(ResetSP->name);
        deleteProperty(CoolerSP->name);
        deleteProperty(ShutterSP->name);
        deleteProperty(CoolerNP->name);
    }

    return true;
}

bool QSICCD::setupParams()
{

    if (isDebug())
        IDLog("In setupParams\n");

    string name,model;
    double temperature;
    double pixel_size_x,pixel_size_y;
    long sub_frame_x,sub_frame_y;
    try
    {
        QSICam.get_Name(name);
        QSICam.get_ModelNumber(model);
        QSICam.get_PixelSizeX(&pixel_size_x);
        QSICam.get_PixelSizeY(&pixel_size_y);
        QSICam.get_NumX(&sub_frame_x);
        QSICam.get_NumY(&sub_frame_y);
        QSICam.get_CCDTemperature(&temperature);
    } catch (std::runtime_error err)
    {
        IDMessage(deviceName(), "Setup Params failed. %s.", err.what());
        if (isDebug())
            IDLog("Setup Params failed. %s.", err.what());
        return false;
    }


    IDMessage(deviceName(), "The CCD Temperature is %f.\n", temperature);

    if (isDebug())
        IDLog("The CCD Temperature is %f.\n", temperature);

    TemperatureN[0].value = temperature;			/* CCD chip temperatre (degrees C) */

    SetCCDParams(sub_frame_x, sub_frame_y, 16, pixel_size_x, pixel_size_y);

    imageWidth  = ImageFrameN[2].value;
    imageHeight = ImageFrameN[3].value;

    IDSetNumber(TemperatureNP, NULL);

    try
    {
     QSICam.get_Name(name);
    } catch (std::runtime_error& err)
    {
        IDMessage(deviceName(), "get_Name() failed. %s.", err.what());
        if (isDebug())
            IDLog("get_Name() failed. %s.\n", err.what());
        return false;
    }
    IDMessage(deviceName(), "%s", name.c_str());

    if (isDebug())
        IDLog("%s\n", name.c_str());

    /*
    int filter_count;
    try {
            QSICam.get_FilterCount(filter_count);
    } catch (std::runtime_error err) {
            IDMessage(deviceName(), "get_FilterCount() failed. %s.", err.what());
            IDLog("get_FilterCount() failed. %s.\n", err.what());
            return;
    }

    IDMessage(deviceName(),"The filter count is %d\n", filter_count);
    IDLog("The filter count is %d\n", filter_count);

    FilterN[0].max = filter_count - 1;
    FilterNP->s = IPS_OK;

    IUUpdateMinMax(&FilterNP);
    IDSetNumber(&FilterNP, "Setting max number of filters.\n");

    FilterSP->s = IPS_OK;
    IDSetSwitch(&FilterSP,NULL);
    */

    QSICam.get_CanAbortExposure(&canAbort);

    if(RawFrame != NULL) delete RawFrame;
    RawFrameSize=XRes*YRes;                 //  this is pixel count
    RawFrameSize=RawFrameSize*2;            //  Each pixel is 2 bytes
    RawFrameSize+=512;                      //  leave a little extra at the end
    RawFrame=new char[RawFrameSize];

    return true;
}

bool QSICCD::ISNewSwitch (const char *dev, const char *name, ISState *states, char *names[], int n)
{

    if(strcmp(dev,deviceName())==0)
    {

         /* Cooler */
        if (!strcmp (name, CoolerSP->name))
        {
          if (IUUpdateSwitch(CoolerSP, states, names, n) < 0) return false;
          activateCooler();
          return true;
        }

        /* Reset */
        if (!strcmp (name, ResetSP->name))
        {
          if (IUUpdateSwitch(ResetSP, states, names, n) < 0) return false;
          resetFrame();
          return true;
        }


        /* Shutter */
        if (!strcmp (name, ShutterSP->name))
        {
            if (IUUpdateSwitch(ShutterSP, states, names, n) < 0) return false;
            shutterControl();
            return true;
        }
    }

        //  Nobody has claimed this, so, ignore it
        return INDI::CCD::ISNewSwitch(dev,name,states,names,n);
}


bool QSICCD::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    INumber *np;

    //  first check if it's for our device
    //IDLog("INDI::CCD::ISNewNumber %s\n",name);
    if(strcmp(dev,deviceName())==0)
    {

        /* Temperature*/
        if (!strcmp(TemperatureRequestNP->name, name))
        {
            TemperatureRequestNP->s = IPS_IDLE;

            np = IUFindNumber(TemperatureRequestNP, names[0]);

            if (!np)
            {
                IDSetNumber(TemperatureRequestNP, "Unknown error. %s is not a member of %s property.", names[0], name);
                return false;
            }

            if (values[0] < MIN_CCD_TEMP || values[0] > MAX_CCD_TEMP)
            {
                IDSetNumber(TemperatureRequestNP, "Error: valid range of temperature is from %d to %d", MIN_CCD_TEMP, MAX_CCD_TEMP);
                return false;
            }

            bool canSetTemp;
            try
            {
                QSICam.get_CanSetCCDTemperature(&canSetTemp);
            } catch (std::runtime_error err)
            {
                IDSetNumber(TemperatureRequestNP, "CanSetCCDTemperature() failed. %s.", err.what());
                if (isDebug())
                    IDLog("CanSetCCDTemperature() failed. %s.", err.what());
                return false;
            }
            if(!canSetTemp)
            {
                IDMessage(deviceName(), "Cannot set CCD temperature, CanSetCCDTemperature == false\n");
                return false;
            }

            try
            {
                QSICam.put_SetCCDTemperature(values[0]);
            } catch (std::runtime_error err)
            {
                IDSetNumber(TemperatureRequestNP, "put_SetCCDTemperature() failed. %s.", err.what());
                if (isDebug())
                    IDLog("put_SetCCDTemperature() failed. %s.", err.what());
                return false;
            }

            TemperatureRequestNP->s = IPS_BUSY;
            TemperatureNP->s = IPS_BUSY;

            IDSetNumber(TemperatureRequestNP, "Setting CCD temperature to %+06.2f C", values[0]);
            if (isDebug())
                IDLog("Setting CCD temperature to %+06.2f C\n", values[0]);
            return true;
        }

    }

    //  if we didn't process it, continue up the chain, let somebody else
    //  give it a shot
    return INDI::CCD::ISNewNumber(dev,name,values,names,n);
}

/*
void ISNewNumber (const char *dev, const char *name, double values[], char *names[], int n)
{


        if (!strcmp(FilterNP->name, name)) {

            targetFilter = values[0];

            np = IUFindNumber(&FilterNP, names[0]);

            if (!np) {
                FilterNP->s = IPS_ALERT;
                IDSetNumber(&FilterNP, "Unknown error. %s is not a member of %s property.", names[0], name);
                return;
            }

            int filter_count;
            try {
                QSICam.get_FilterCount(filter_count);
            } catch (std::runtime_error err) {
                IDSetNumber(&FilterNP, "get_FilterCount() failed. %s.", err.what());
            }
            if (targetFilter < FIRST_FILTER || targetFilter > filter_count - 1) {
                FilterNP->s = IPS_ALERT;
                IDSetNumber(&FilterNP, "Error: valid range of filter is from %d to %d", FIRST_FILTER, LAST_FILTER);
                return;
            }

            IUUpdateNumber(&FilterNP, values, names, n);

            FilterNP->s = IPS_BUSY;
            IDSetNumber(&FilterNP, "Setting current filter to slot %d", targetFilter);
            IDLog("Setting current filter to slot %d\n", targetFilter);

            try {
                QSICam.put_Position(targetFilter);
            } catch(std::runtime_error err) {
                FilterNP->s = IPS_ALERT;
                IDSetNumber(&FilterNP, "put_Position() failed. %s.", err.what());
                IDLog("put_Position() failed. %s.", err.what());
                return;
            }



            short newFilter;
            try {
                QSICam.get_Position(&newFilter);
            } catch(std::runtime_error err) {
                FilterNP->s = IPS_ALERT;
                IDSetNumber(&FilterNP, "get_Position() failed. %s.", err.what());
                IDLog("get_Position() failed. %s.\n", err.what());
                return;
            }

            if (newFilter == targetFilter) {
                FilterN[0].value = targetFilter;
                FilterNP->s = IPS_OK;
                IDSetNumber(&FilterNP, "Filter set to slot #%d", targetFilter);
                return;
            }

            return;
        }


}
*/

int QSICCD::StartExposure(float duration)
{
        double minDuration;
        bool shortExposure = false;

        try
        {
            QSICam.get_MinExposureTime(&minDuration);
        } catch (std::runtime_error err)
        {
            IDMessage(deviceName(), "get_MinExposureTime() failed. %s.", err.what());
            if (isDebug())
                IDLog("get_MinExposureTime() failed. %s.", err.what());
            return false;
        }

        if(duration < minDuration)
        {
            duration = minDuration;
            IDMessage(deviceName(), "Exposure shorter than minimum duration %g s requested. \n Setting exposure time to %g s.", minDuration,minDuration);
        }

        imageFrameType = FrameType;

        if(imageFrameType == BIAS_FRAME)
        {
            ImageExposureN[0].value =  minDuration;
            IDSetNumber(ImageExposureNP, "Bias Frame (s) : %g\n", minDuration);
            if (isDebug())
                IDLog("Bias Frame (s) : %g\n", minDuration);
        } else
        {
            ImageExposureN[0].value = duration;
            if (isDebug())
                IDLog("Exposure Time (s) is: %g\n", duration);
        }

         imageExpose = ImageExposureN[0].value;

            /* BIAS frame is the same as DARK but with minimum period. i.e. readout from camera electronics.*/
            if (imageFrameType == BIAS_FRAME)
            {
                try
                {
                    double minDuration;
                    QSICam.get_MinExposureTime(&minDuration);
                    QSICam.put_PreExposureFlush(QSICamera::FlushNormal);
                    QSICam.StartExposure (minDuration,false);
                    shortExposure = true;
                } catch (std::runtime_error& err)
                {
                    ImageExposureNP->s = IPS_ALERT;
                    IDMessage(deviceName(), "StartExposure() failed. %s.", err.what());
                    if (isDebug())
                        IDLog("StartExposure() failed. %s.\n", err.what());
                    return -1;
                }
            }

            else if (imageFrameType == DARK_FRAME)
            {
                try
                {
                    QSICam.put_PreExposureFlush(QSICamera::FlushNormal);
                    QSICam.StartExposure (imageExpose,false);
                } catch (std::runtime_error& err)
                {
                    ImageExposureNP->s = IPS_ALERT;
                    IDMessage(deviceName(), "StartExposure() failed. %s.", err.what());
                    if (isDebug())
                        IDLog("StartExposure() failed. %s.\n", err.what());
                    return -1;
                }
            }

            else if (imageFrameType == LIGHT_FRAME || imageFrameType == FLAT_FRAME)
            {
                try
                {
                    QSICam.put_PreExposureFlush(QSICamera::FlushNormal);
                    QSICam.StartExposure (imageExpose,true);
                } catch (std::runtime_error& err)
                {
                    ImageExposureNP->s = IPS_ALERT;
                    IDMessage(deviceName(), "StartExposure() failed. %s.", err.what());
                    if (isDebug())
                        IDLog("StartExposure() failed. %s.\n", err.what());
                    return false;
                }
            }

            ImageExposureNP->s = IPS_BUSY;
            gettimeofday(&ExpStart,NULL);
            IDSetNumber(ImageExposureNP, "Taking a %g seconds frame...", imageExpose);

            //ExposeTimeLeftN[0].value = QSIImg->timeLeft();
            //ExposeTimeLeftNP->s = IPS_BUSY;
            //IDSetNumber(&ExposeTimeLeftNP,NULL);
            if (isDebug())
                IDLog("Taking a frame...\n");

            return (shortExposure ? 1 : 0);
}

bool QSICCD::AbortExposure()
{
    if (canAbort)
    {
        try
        {
          QSICam.AbortExposure();
        } catch (std::runtime_error err)
        {
          ImageExposureNP->s = IPS_ALERT;
          IDSetNumber(ImageExposureNP, "AbortExposure() failed. %s.", err.what());
          IDLog("AbortExposure() failed. %s.\n", err.what());
          return false;
        }

        ImageExposureNP->s = IPS_IDLE;
        IDSetNumber(ImageExposureNP, NULL);
        return true;
    }

    return false;
}

float QSICCD::CalcTimeLeft(timeval start,float req)
{
    double timesince;
    double timeleft;
    struct timeval now;
    gettimeofday(&now,NULL);

    timesince=(double)(now.tv_sec * 1000.0 + now.tv_usec/1000) - (double)(start.tv_sec * 1000.0 + start.tv_usec/1000);
    timesince=timesince/1000;
    timeleft=req-timesince;
    return timeleft;
}

void QSICCD::updateCCDFrame()
{
    char errmsg[ERRMSG_SIZE];

    /* Add the X and Y offsets */
    long x_1 = ImageFrameN[0].value;
    long y_1 = ImageFrameN[1].value;

    long x_2 = x_1 + (ImageFrameN[2].value / ImageBinN[0].value);
    long y_2 = y_1 + (ImageFrameN[3].value / ImageBinN[1].value);

    long sensorPixelSize_x,
         sensorPixelSize_y;
    try
    {
        QSICam.get_CameraXSize(&sensorPixelSize_x);
        QSICam.get_CameraYSize(&sensorPixelSize_y);
    } catch (std::runtime_error err)
    {
        IDMessage(deviceName(), "Getting image area size failed. %s.", err.what());
    }

    if (x_2 > sensorPixelSize_x / ImageBinN[0].value)
        x_2 = sensorPixelSize_x / ImageBinN[0].value;

    if (y_2 > sensorPixelSize_y / ImageBinN[1].value)
        y_2 = sensorPixelSize_y / ImageBinN[1].value;

    if (isDebug())
        IDLog("The Final image area is (%ld, %ld), (%ld, %ld)\n", x_1, y_1, x_2, y_2);

    imageWidth  = x_2 - x_1;
    imageHeight = y_2 - y_1;

    try
    {
        QSICam.put_StartX(x_1);
        QSICam.put_StartY(y_1);
        QSICam.put_NumX(imageWidth);
        QSICam.put_NumY(imageHeight);
    } catch (std::runtime_error err)
    {
        snprintf(errmsg, ERRMSG_SIZE, "Setting image area failed. %s.\n",err.what());
        IDMessage(deviceName(), "Setting image area failed. %s.", err.what());
        if (isDebug())
            IDLog("Setting image area failed. %s.", err.what());
        return;
    }
}

void QSICCD::updateCCDBin()
{
    try
    {
        QSICam.put_BinX(ImageBinN[0].value);
    } catch (std::runtime_error err)
    {
        IDSetNumber(ImageBinNP, "put_BinX() failed. %s.", err.what());
        IDLog("put_BinX() failed. %s.", err.what());
        return;
    }

    try
    {
        QSICam.put_BinY(ImageBinN[1].value);
    } catch (std::runtime_error err)
    {
        IDSetNumber(ImageBinNP, "put_BinY() failed. %s.", err.what());
        IDLog("put_BinY() failed. %s.", err.what());
        return;
    }

    updateCCDFrame();
}

/* Downloads the image from the CCD.
 N.B. No processing is done on the image */
int QSICCD::grabImage()
{

	int fd;
	char errmsg[ERRMSG_SIZE];
        unsigned short* image = (unsigned short *) RawFrame;

        memset(RawFrame,0,RawFrameSize);

        int x,y,z;
        try {
            bool imageReady = false;
            QSICam.get_ImageReady(&imageReady);
            while(!imageReady)
            {
                usleep(500);
                QSICam.get_ImageReady(&imageReady);
            }

            QSICam.get_ImageArraySize(x,y,z);
            QSICam.get_ImageArray(image);
            imageBuffer = image;
            imageWidth  = x;
            imageHeight = y;
        } catch (std::runtime_error err)
        {
            IDMessage(deviceName(), "get_ImageArray() failed. %s.", err.what());
            IDLog("get_ImageArray() failed. %s.", err.what());
            return -1;
        }

        IDMessage(deviceName(), "Download complete.\n");
	
        ExposureComplete();

	return 0;
}

void QSICCD::addFITSKeywords(fitsfile *fptr)
{

        int status=0; 
        char binning_s[32];
        char frame_s[32];
        double min_val = min();
        double max_val = max();
        double exposure = 1000 * imageExpose; // conversion s - ms

        snprintf(binning_s, 32, "(%g x %g)", ImageBinN[0].value, ImageBinN[1].value);

        switch (imageFrameType)
        {
          case LIGHT_FRAME:
      	    strcpy(frame_s, "Light");
	    break;
          case BIAS_FRAME:
            strcpy(frame_s, "Bias");
	    break;
          case FLAT_FRAME:
            strcpy(frame_s, "Flat Field");
	    break;
          case DARK_FRAME:
            strcpy(frame_s, "Dark");
	    break;
        }

        char name_s[32] = "QSI";
        double electronsPerADU;
        short filter;
        try {
            string name;
            QSICam.get_Name(name);
            for(unsigned i = 0; i < 18; ++i) name_s[i] = name[i];
            QSICam.get_ElectronsPerADU(&electronsPerADU);
            QSICam.get_Position(&filter);
        } catch (std::runtime_error& err) {
            IDMessage(deviceName(), "get_Name() failed. %s.", err.what());
            IDLog("get_Name() failed. %s.\n", err.what());
            return;
        }

        fits_update_key_s(fptr, TDOUBLE, "CCD-TEMP", &(TemperatureN[0].value), "CCD Temperature (Celcius)", &status);
        fits_update_key_s(fptr, TDOUBLE, "EXPTIME", &(imageExpose), "Total Exposure Time (s)", &status);
        if(imageFrameType == DARK_FRAME)
        fits_update_key_s(fptr, TDOUBLE, "DARKTIME", &(imageExpose), "Total Exposure Time (s)", &status);
        fits_update_key_s(fptr, TDOUBLE, "PIX-SIZ", &(ImagePixelSizeN[0].value), "Pixel Size (microns)", &status);
        fits_update_key_s(fptr, TSTRING, "BINNING", binning_s, "Binning HOR x VER", &status);
        fits_update_key_s(fptr, TSTRING, "FRAME", frame_s, "Frame Type", &status);
        fits_update_key_s(fptr, TDOUBLE, "DATAMIN", &min_val, "Minimum value", &status);
        fits_update_key_s(fptr, TDOUBLE, "DATAMAX", &max_val, "Maximum value", &status);
        fits_update_key_s(fptr, TSTRING, "INSTRUME", name_s, "CCD Name", &status);
        fits_update_key_s(fptr, TDOUBLE, "EPERADU", &electronsPerADU, "Electrons per ADU", &status);

        fits_write_date(fptr, &status);
}

void QSICCD::fits_update_key_s(fitsfile* fptr, int type, string name, void* p, string explanation, int* status)
{
        // this function is for removing warnings about deprecated string conversion to char* (from arg 5)
        fits_update_key(fptr,type,name.c_str(),p, const_cast<char*>(explanation.c_str()), status);
}


int QSICCD::manageDefaults(char errmsg[])
{

        long err;
  
        /* X horizontal binning */
        try
        {
            QSICam.put_BinX(ImageBinN[0].value);
        } catch (std::runtime_error err) {
            IDMessage(deviceName(), "Error: put_BinX() failed. %s.", err.what());
            IDLog("Error: put_BinX() failed. %s.\n", err.what());
            return -1;
        }

        /* Y vertical binning */
        try {
            QSICam.put_BinY(ImageBinN[1].value);
        } catch (std::runtime_error err)
        {
            IDMessage(deviceName(), "Error: put_BinY() failed. %s.", err.what());
            IDLog("Error: put_BinX() failed. %s.\n", err.what());
            return -1;
        }

        IDLog("Setting default binning %f x %f.\n", ImageBinN[0].value, ImageBinN[1].value);

        updateCCDFrame();

        /*
        short current_filter;
        try
        {
            QSICam.put_Position(targetFilter);
            QSICam.get_Position(&current_filter);
        } catch (std::runtime_error err)
        {
            IDMessage(deviceName(), "QSICamera::get_FilterPos() failed. %s.", err.what());
            IDLog("QSICamera::get_FilterPos() failed. %s.\n", err.what());
            errmsg = const_cast<char*>(err.what());
            return true;
        }

        IDMessage(deviceName(),"The current filter is %d\n", current_filter);
        IDLog("The current filter is %d\n", current_filter);

        FilterN[0].value = current_filter;
        IDSetNumber(&FilterNP, "Storing defaults");
        */

        /* Success */
        return 0;
}


bool QSICCD::Connect(char *msg)
{
    bool connected;

    if (isDebug())
    {
        IDLog ("Connecting CCD\n");
        IDLog("Attempting to find the camera\n");
    }


            try
            {
                QSICam.get_Connected(&connected);
            } catch (std::runtime_error err)
            {
                IDMessage(deviceName(), "Error: get_Connected() failed. %s.", err.what());
                snprintf(msg, MAXRBUF, "Error: get_Connected() failed. %s.", err.what());
                if (isDebug())
                    IDLog("%s\n", msg);
                return false;
            }

            if(!connected)
            {
                try
                {
                    QSICam.put_Connected(true);
                } catch (std::runtime_error err)
                {
                    snprintf(msg, MAXRBUF, "Error: put_Connected(true) failed. %s.", err.what());
                    if (isDebug())
                        IDLog("%s\n", msg);
                    return false;
                }
            }

            /* Success! */
            snprintf(msg, MAXRBUF, "CCD is online. Retrieving basic data.");
            //IDSetSwitch(&ConnectSP, "CCD is online. Retrieving basic data.");
            if (isDebug())
                IDLog("%s\n", msg);

            setupParams();

            if (manageDefaults(msg))
            {
                IDMessage(deviceName(), msg, NULL);
                if (isDebug())
                    IDLog("%s\n", msg);
                return false;
            }

            SetTimer(POLLMS);
            return true;
}


bool QSICCD::Disconnect()
{
    char msg[MAXRBUF];
    bool connected;
    delete RawFrame;
    RawFrameSize=0;
    RawFrame=NULL;


    try
    {
        QSICam.get_Connected(&connected);
    } catch (std::runtime_error err)
    {
        IDMessage(deviceName(), "Error: get_Connected() failed. %s.", err.what());
        snprintf(msg, MAXRBUF, "Error: get_Connected() failed. %s.", err.what());

        if (isDebug())
            IDLog("%s\n", msg);
        return false;
    }

    if (connected)
    {
        try
        {
            QSICam.put_Connected(false);
        } catch (std::runtime_error err)
        {
            IDMessage(deviceName(), "Error: put_Connected(false) failed. %s.", err.what());
            snprintf(msg, MAXRBUF, "Error: put_Connected(false) failed. %s.\n", err.what());
            if (isDebug())
                IDLog("%s\n", msg);
            return false;
        }
    }

    IDMessage(deviceName(), "CCD is offline.");
    return true;
}

void QSICCD::activateCooler()
{

        bool coolerOn;

        switch (CoolerS[0].s)
        {
          case ISS_ON:
            try
            {
                QSICam.get_CoolerOn(&coolerOn);
            } catch (std::runtime_error err)
            {
                CoolerSP->s = IPS_IDLE;
                CoolerS[0].s = ISS_OFF;
                CoolerS[1].s = ISS_ON;
                IDSetSwitch(CoolerSP, "Error: CoolerOn() failed. %s.", err.what());
                IDLog("Error: CoolerOn() failed. %s.\n", err.what());
                return;
            }

            if(!coolerOn)
            {
                try
                {
                    QSICam.put_CoolerOn(true);
                } catch (std::runtime_error err)
                {
                    CoolerSP->s = IPS_IDLE;
                    CoolerS[0].s = ISS_OFF;
                    CoolerS[1].s = ISS_ON;
                    IDSetSwitch(CoolerSP, "Error: put_CoolerOn(true) failed. %s.", err.what());
                    IDLog("Error: put_CoolerOn(true) failed. %s.\n", err.what());
                    return;
                }
            }

            /* Success! */
            CoolerS[0].s = ISS_ON;
            CoolerS[1].s = ISS_OFF;
            CoolerSP->s = IPS_OK;
            IDSetSwitch(CoolerSP, "Cooler ON\n");
            IDLog("Cooler ON\n");
            break;

          case ISS_OFF:
              CoolerS[0].s = ISS_OFF;
              CoolerS[1].s = ISS_ON;
              CoolerSP->s = IPS_IDLE;

              try
              {
                 QSICam.get_CoolerOn(&coolerOn);
                 if(coolerOn) QSICam.put_CoolerOn(false);
              } catch (std::runtime_error err)
              {
                 IDSetSwitch(CoolerSP, "Error: CoolerOn() failed. %s.", err.what());
                 IDLog("Error: CoolerOn() failed. %s.\n", err.what());
                 return;                                              
              }
              IDSetSwitch(CoolerSP, "Cooler is OFF.");
              break;
        }
}

double QSICCD::min()
{

        double lmin = imageBuffer[0];
        int ind=0, i, j;
  
        for (i= 0; i < imageHeight ; i++)
            for (j= 0; j < imageWidth; j++) {
                ind = (i * imageWidth) + j;
                if (imageBuffer[ind] < lmin) lmin = imageBuffer[ind];
        }
    
        return lmin;
}

double QSICCD::max()
{

        double lmax = imageBuffer[0];
        int ind=0, i, j;
  
        for (i= 0; i < imageHeight ; i++)
            for (j= 0; j < imageWidth; j++) {
                ind = (i * imageWidth) + j;
                if (imageBuffer[ind] > lmax) lmax = imageBuffer[ind];
        }
    
        return lmax;
}

void QSICCD::resetFrame()
{

        long sensorPixelSize_x,
             sensorPixelSize_y;
        try {
            QSICam.get_CameraXSize(&sensorPixelSize_x);
            QSICam.get_CameraYSize(&sensorPixelSize_y);
        } catch (std::runtime_error err)
        {
            IDMessage(deviceName(), "Getting image area size failed. %s.", err.what());
        }

        imageWidth  = sensorPixelSize_x;
        imageHeight = sensorPixelSize_y;


        try
        {
            QSICam.put_BinX(1);
            QSICam.put_BinY(1);
        } catch (std::runtime_error err)
        {
            IDSetNumber(ImageBinNP, "Resetting BinX/BinY failed. %s.", err.what());
            IDLog("Resetting BinX/BinY failed. %s.", err.what());
            return;
        }

        SetCCDParams(imageWidth, imageHeight, 16, 1, 1);

        ResetSP->s = IPS_IDLE;
        IDSetSwitch(ResetSP, "Resetting frame and binning.");

        char errmsg[ERRMSG_SIZE];

        updateCCDFrame();

        return;
}

/*
void turnWheel() {

        short current_filter;

        switch (FilterS[0]->s) {
          case ISS_ON:
            if(current_filter < LAST_FILTER) current_filter++;
            else current_filter = FIRST_FILTER;
            try {
                QSICam.get_Position(&current_filter);
                if(current_filter < LAST_FILTER) current_filter++;
                else current_filter = FIRST_FILTER;
                QSICam.put_Position(current_filter);
            } catch (std::runtime_error err) {
                FilterSP->s = IPS_IDLE;
                FilterS[0]->s = ISS_OFF;
                FilterS[1]->s = ISS_OFF;
                IDMessage(deviceName(), "QSICamera::get_FilterPos() failed. %s.", err.what());
                IDLog("QSICamera::get_FilterPos() failed. %s.\n", err.what());
                return;
            }

            FilterN[0].value = current_filter;
            FilterS[0]->s = ISS_OFF;
            FilterS[1]->s = ISS_OFF;
            FilterSP->s = IPS_OK;
            IDSetSwitch(&FilterSP,"The current filter is %d\n", current_filter);
            IDLog("The current filter is %d\n", current_filter);

            break;

          case ISS_OFF:
            try {
                QSICam.get_Position(&current_filter);
                if(current_filter > FIRST_FILTER) current_filter--;
                else current_filter = LAST_FILTER;
                QSICam.put_Position(current_filter);
            } catch (std::runtime_error err) {
                FilterSP->s = IPS_IDLE;
                FilterS[0]->s = ISS_OFF;
                FilterS[1]->s = ISS_OFF;
                IDMessage(deviceName(), "QSICamera::get_FilterPos() failed. %s.", err.what());
                IDLog("QSICamera::get_FilterPos() failed. %s.\n", err.what());
                return;
            }

            FilterN[0].value = current_filter;
            FilterS[0]->s = ISS_OFF;
            FilterS[1]->s = ISS_OFF;
            FilterSP->s = IPS_OK;
            IDSetSwitch(&FilterSP,"The current filter is %d\n", current_filter);
            IDLog("The current filter is %d\n", current_filter);

            break;
        }
}
*/

void QSICCD::shutterControl()
{

        bool hasShutter;
        try{
            QSICam.get_HasShutter(&hasShutter);
        }catch (std::runtime_error err) {
            ShutterSP->s   = IPS_IDLE;
            ShutterS[0].s = ISS_OFF;
            ShutterS[1].s = ISS_OFF;
            IDMessage(deviceName(), "QSICamera::get_HasShutter() failed. %s.", err.what());
            IDLog("QSICamera::get_HasShutter() failed. %s.\n", err.what());
            return;
        }

        if(hasShutter){
            switch (ShutterS[0].s)
            {
              case ISS_ON:

                try
            {
                    QSICam.put_ManualShutterMode(true);
                    QSICam.put_ManualShutterOpen(true);
                }catch (std::runtime_error err)
                {
                    ShutterSP->s = IPS_IDLE;
                    ShutterS[0].s = ISS_OFF;
                    ShutterS[1].s = ISS_ON;
                    IDSetSwitch(ShutterSP, "Error: ManualShutterOpen() failed. %s.", err.what());
                    IDLog("Error: ManualShutterOpen() failed. %s.\n", err.what());
                    return;
                }

                /* Success! */
                ShutterS[0].s = ISS_ON;
                ShutterS[1].s = ISS_OFF;
                ShutterSP->s = IPS_OK;
                IDSetSwitch(ShutterSP, "Shutter opened manually.");
                IDLog("Shutter opened manually.\n");

                break;

              case ISS_OFF:

                try{
                    QSICam.put_ManualShutterOpen(false);
                    QSICam.put_ManualShutterMode(false);
                }catch (std::runtime_error err)
                {
                    ShutterSP->s = IPS_IDLE;
                    ShutterS[0].s = ISS_ON;
                    ShutterS[1].s = ISS_OFF;
                    IDSetSwitch(ShutterSP, "Error: ManualShutterOpen() failed. %s.", err.what());
                    IDLog("Error: ManualShutterOpen() failed. %s.\n", err.what());
                    return;
                }

                /* Success! */
                ShutterS[0].s = ISS_OFF;
                ShutterS[1].s = ISS_ON;
                ShutterSP->s = IPS_IDLE;
                IDSetSwitch(ShutterSP, "Shutter closed manually.");
                IDLog("Shutter closed manually.\n");

                break;
            }
        }
}


void QSICCD::TimerHit()
{
    long err;
    long timeleft;
    double ccdTemp;
    double coolerPower;


    if(isConnected() == false)
        return;  //  No need to reset timer if we are not connected anymore

    switch (ImageExposureNP->s)
    {
      case IPS_IDLE:
        break;

      case IPS_OK:
        break;

      case IPS_BUSY:

        bool imageReady;
        try
       {
        if (isDebug())
            IDLog("Trying to see if the image is ready.\n");

            QSICam.get_ImageReady(&imageReady);
        } catch (std::runtime_error err)
        {
            ImageExposureNP->s = IPS_ALERT;

            IDSetNumber(ImageExposureNP, "get_ImageReady() failed. %s.", err.what());
            IDLog("get_ImageReady() failed. %s.", err.what());
            break;
        }

        timeleft=CalcTimeLeft(ExpStart,imageExpose);

        if (timeleft < 0)
            timeleft = 0;

        ImageExposureN[0].value = timeleft;

        if (isDebug())
            IDLog("With time left %ld\n", timeleft);

        if (!imageReady)
        {
            IDSetNumber(ImageExposureNP, NULL);
            if (isDebug())
                IDLog("image not yet ready....\n");
            break;
        }

        /* We're done exposing */
        ImageExposureNP->s = IPS_OK;
        IDSetNumber(ImageExposureNP, "Exposure done, downloading image...");
        IDLog("Exposure done, downloading image...\n");

        /* grab and save image */
        grabImage();

        break;

      case IPS_ALERT:
        break;
    }

    switch (TemperatureNP->s)
    {
      case IPS_IDLE:
      case IPS_OK:
        try
       {
            QSICam.get_CCDTemperature(&ccdTemp);
        } catch (std::runtime_error err)
        {
            TemperatureNP->s = IPS_IDLE;
            IDSetNumber(TemperatureNP, "get_CCDTemperature() failed. %s.", err.what());
            IDLog("get_CCDTemperature() failed. %s.", err.what());
            return;
        }

        if (fabs(TemperatureN[0].value - ccdTemp) >= TEMP_THRESHOLD)
        {
           TemperatureN[0].value = ccdTemp;
           IDSetNumber(TemperatureNP, NULL);
        }
        break;

      case IPS_BUSY:
        try
        {
            QSICam.get_CCDTemperature(&ccdTemp);
        } catch (std::runtime_error err)
        {
            TemperatureNP->s = IPS_ALERT;
            IDSetNumber(TemperatureNP, "get_CCDTemperature() failed. %s.", err.what());
            IDLog("get_CCDTemperature() failed. %s.", err.what());
            return;
        }

        if (fabs(TemperatureN[0].value - ccdTemp) <= TEMP_THRESHOLD)
        {
            TemperatureNP->s = IPS_OK;
            TemperatureRequestNP->s = IPS_OK;

            IDSetNumber(TemperatureRequestNP, NULL);
        }

        TemperatureN[0].value = ccdTemp;
        IDSetNumber(TemperatureNP, NULL);
        break;

      case IPS_ALERT:
        break;
    }

    switch (CoolerNP->s)
    {
      case IPS_IDLE:
      case IPS_OK:
        try
        {
            QSICam.get_CoolerPower(&coolerPower);
        } catch (std::runtime_error err)
        {
            CoolerNP->s = IPS_IDLE;
            IDSetNumber(CoolerNP, "get_CoolerPower() failed. %s.", err.what());
            IDLog("get_CoolerPower() failed. %s.", err.what());
            return;
        }

        if (CoolerN[0].value != coolerPower)
        {
            CoolerN[0].value = coolerPower;
            IDSetNumber(CoolerNP, NULL);
        }

        break;

      case IPS_BUSY:
        try
       {
            QSICam.get_CoolerPower(&coolerPower);
        } catch (std::runtime_error err)
        {
            CoolerNP->s = IPS_ALERT;
            IDSetNumber(CoolerNP, "get_CoolerPower() failed. %s.", err.what());
            IDLog("get_CoolerPower() failed. %s.", err.what());
            return;
        }
        CoolerNP->s = IPS_OK;

        CoolerN[0].value = coolerPower;
        IDSetNumber(CoolerNP, NULL);
        break;

      case IPS_ALERT:
         break;
    }

    switch (ResetSP->s)
    {
      case IPS_IDLE:
         break;
      case IPS_OK:
         break;
      case IPS_BUSY:
         break;
      case IPS_ALERT:
         break;
    }

    /*
    switch (FilterNP->s)
    {

      case IPS_IDLE:
      case IPS_OK:

        short current_filter;
        try
       {
            QSICam.get_Position(&current_filter);
        } catch (std::runtime_error err)
        {
            FilterNP->s = IPS_ALERT;
            IDSetNumber(&FilterNP, "QSICamera::get_FilterPos() failed. %s.", err.what());
            IDLog("QSICamera::get_FilterPos() failed. %s.\n", err.what());
            return;
        }

        if (FilterN[0].value != current_filter)
        {
            FilterN[0].value = current_filter;
            IDSetNumber(&FilterNP, NULL);
        }
        break;

      case IPS_BUSY:
        break;

      case IPS_ALERT:
        break;
    }
    */



    SetTimer(POLLMS);
    return;
}