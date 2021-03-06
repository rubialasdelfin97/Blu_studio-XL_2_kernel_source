/*
 * FM50AF voice coil motor driver
 *
 *
 */

#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/fs.h>

#include "lens_info.h"


#define AF_DRVNAME "FM50AF_DRV"
#define AF_I2C_SLAVE_ADDR        0x18

#define AF_DEBUG
#ifdef AF_DEBUG
#define LOG_INF(format, args...) pr_debug(AF_DRVNAME " [%s] " format, __func__, ##args)
#else
#define LOG_INF(format, args...)
#endif


static struct i2c_client *g_pstAF_I2Cclient;
static int *g_pAF_Opened;
static spinlock_t *g_pAF_SpinLock;


static unsigned long g_u4AF_INF;
static unsigned long g_u4AF_MACRO = 1023;
static unsigned long g_u4TargetPosition;
static unsigned long g_u4CurrPosition;

static int g_SR = 5;
static int g_FM50AF_addr_switch = 0;

static int s4AF_ReadReg(unsigned short *a_pu2Result)
{
	int i4RetValue = 0;
	char pBuff[2];

	if (g_FM50AF_addr_switch==0)
	{
		pBuff[0] = 0x03;
		g_pstAF_I2Cclient->addr = 0x1c>>1;
		i4RetValue = i2c_master_send(g_pstAF_I2Cclient, pBuff, 1);

		if (i4RetValue < 0) 
	  	{
		    LOG_INF("[FM50AF] I2C write failed!! g_FM50AF_addr_switch=%x\n",g_FM50AF_addr_switch);
			return -1;
		}	
		i4RetValue = i2c_master_recv(g_pstAF_I2Cclient, pBuff , 2);

		if (i4RetValue < 0) 
		{
			g_FM50AF_addr_switch=1;
			LOG_INF("[FM50AF] I2C write failed!! g_FM50AF_addr_switch=%x\n",g_FM50AF_addr_switch);
			return -1;
		}
		*a_pu2Result = (((u16)pBuff[0]) << 8) + pBuff[1];
	}
    else
	{
		g_pstAF_I2Cclient->addr = AF_I2C_SLAVE_ADDR;

		g_pstAF_I2Cclient->addr = g_pstAF_I2Cclient->addr >> 1;

		i4RetValue = i2c_master_recv(g_pstAF_I2Cclient, pBuff, 2);

		if (i4RetValue < 0) {
			LOG_INF("I2C read failed!!\n");
			return -1;
		}

		*a_pu2Result = (((u16) pBuff[0]) << 4) + (pBuff[1] >> 4);
	}

	return 0;
}

static int s4AF_WriteReg(u16 a_u2Data)
{
	int i4RetValue = 0;
	char puSendCmd[4];
	if (g_FM50AF_addr_switch==0)
	{
			puSendCmd[0] = 0x03;
			puSendCmd[1] = (char)(a_u2Data >> 8);
			puSendCmd[2] = (char)(a_u2Data & 0xFF);
			g_pstAF_I2Cclient->addr = 0x1c>>1; 
			
			g_pstAF_I2Cclient->ext_flag |= I2C_A_FILTER_MSG;
			i4RetValue = i2c_master_send(g_pstAF_I2Cclient, puSendCmd, 3);
			if (i4RetValue < 0) 
			{
				LOG_INF("[FM50AF] I2C read failed!! g_FM50AF_addr_switch=%x\n",g_FM50AF_addr_switch);
				return -1;
			}

	}
	else
	{

		puSendCmd[0] = (char)(a_u2Data >> 4);
		puSendCmd[1] = (char)(((a_u2Data & 0xF) << 4) + g_SR);
		g_pstAF_I2Cclient->addr = AF_I2C_SLAVE_ADDR;

		g_pstAF_I2Cclient->addr = g_pstAF_I2Cclient->addr >> 1;
		LOG_INF("[FM50AF] a_u2Data=%d.\n",a_u2Data);
		i4RetValue = i2c_master_send(g_pstAF_I2Cclient, puSendCmd, 2);

		if (i4RetValue < 0) {
			LOG_INF("I2C send failed!!\n");
			return -1;
		}
	}
	return 0;
}

static inline int getAFInfo(__user stAF_MotorInfo *pstMotorInfo)
{
	stAF_MotorInfo stMotorInfo;

	stMotorInfo.u4MacroPosition = g_u4AF_MACRO;
	stMotorInfo.u4InfPosition = g_u4AF_INF;
	stMotorInfo.u4CurrentPosition = g_u4CurrPosition;
	stMotorInfo.bIsSupportSR = 1;

	stMotorInfo.bIsMotorMoving = 1;

	if (*g_pAF_Opened >= 1)
		stMotorInfo.bIsMotorOpen = 1;
	else
		stMotorInfo.bIsMotorOpen = 0;

	if (copy_to_user(pstMotorInfo, &stMotorInfo, sizeof(stAF_MotorInfo)))
		LOG_INF("copy to user failed when getting motor information\n");

	return 0;
}

static inline int moveAF(unsigned long a_u4Position)
{
	int ret = 0;

	if ((a_u4Position > g_u4AF_MACRO) || (a_u4Position < g_u4AF_INF)) {
		LOG_INF("out of range\n");
		return -EINVAL;
	}

	if (*g_pAF_Opened == 1) {
		unsigned short InitPos;

		ret = s4AF_ReadReg(&InitPos);

		if (ret == 0) {
			LOG_INF("Init Pos %6d\n", InitPos);

			spin_lock(g_pAF_SpinLock);
			g_u4CurrPosition = (unsigned long)InitPos;
			spin_unlock(g_pAF_SpinLock);

		} else {
			spin_lock(g_pAF_SpinLock);
			g_u4CurrPosition = 0;
			spin_unlock(g_pAF_SpinLock);
		}

		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 2;
		spin_unlock(g_pAF_SpinLock);
	}

	if (g_u4CurrPosition == a_u4Position)
		return 0;

	spin_lock(g_pAF_SpinLock);
	g_u4TargetPosition = a_u4Position;
	g_SR = 5;
	spin_unlock(g_pAF_SpinLock);

	/* LOG_INF("move [curr] %d [target] %d\n", g_u4CurrPosition, g_u4TargetPosition); */


	if (s4AF_WriteReg((unsigned short)g_u4TargetPosition) == 0) {
		spin_lock(g_pAF_SpinLock);
		g_u4CurrPosition = (unsigned long)g_u4TargetPosition;
		spin_unlock(g_pAF_SpinLock);
	} else {
		LOG_INF("set I2C failed when moving the motor\n");
	}

	return 0;
}

static inline int setAFInf(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_INF = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	return 0;
}

static inline int setAFMacro(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_MACRO = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	return 0;
}

/* ////////////////////////////////////////////////////////////// */
long FM50AF_Ioctl(struct file *a_pstFile, unsigned int a_u4Command, unsigned long a_u4Param)
{
	long i4RetValue = 0;

	switch (a_u4Command) {
	case AFIOC_G_MOTORINFO:
		i4RetValue = getAFInfo((__user stAF_MotorInfo *) (a_u4Param));
		break;

	case AFIOC_T_MOVETO:
		i4RetValue = moveAF(a_u4Param);
		break;

	case AFIOC_T_SETINFPOS:
		i4RetValue = setAFInf(a_u4Param);
		break;

	case AFIOC_T_SETMACROPOS:
		i4RetValue = setAFMacro(a_u4Param);
		break;

	default:
		LOG_INF("No CMD\n");
		i4RetValue = -EPERM;
		break;
	}

	return i4RetValue;
}

/* 1.Deallocate anything that "open" allocated in private_data. */
/* 2.Shut down the device on last close. */
/* 3.Only called once on last time. */
/* Q1 : Try release multiple times. */
int FM50AF_Release(struct inode *a_pstInode, struct file *a_pstFile)
{
	LOG_INF("Start\n");

	if (*g_pAF_Opened == 2) {
		LOG_INF("Wait\n");
		g_SR = 5;
		s4AF_WriteReg(200);
		msleep(20);
		s4AF_WriteReg(100);
		msleep(20);
	}

	if (*g_pAF_Opened) {
		LOG_INF("Free\n");

		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 0;
		spin_unlock(g_pAF_SpinLock);
	}

	LOG_INF("End\n");

	return 0;
}

static int s4FM50AF_enable(int enable)
{
    int  i4RetValue = 0;

    char puSendCmd[2] = {0x02,0x19};
    g_pstAF_I2Cclient->addr = 0x1c>>1;  //add by hxq
    g_pstAF_I2Cclient->ext_flag |= I2C_A_FILTER_MSG;
	if(enable)
	{
		puSendCmd[1] = 0x19;
	}
	else
	{
		puSendCmd[1] = 0x00;
	}
    i4RetValue = i2c_master_send(g_pstAF_I2Cclient, puSendCmd, 2);
    
    if (i4RetValue < 0) 
    {
        LOG_INF("[FM50AF] liupan s4FM50AF_enable I2C send failed!! \n");
        return -1;
    }

    return 0;
}
void FM50AF_SetI2Cclient(struct i2c_client *pstAF_I2Cclient, spinlock_t *pAF_SpinLock, int *pAF_Opened)
{
	int iic_ret=0;
	g_pstAF_I2Cclient = pstAF_I2Cclient;
	g_pAF_SpinLock = pAF_SpinLock;
	g_pAF_Opened = pAF_Opened;
	
	iic_ret = s4FM50AF_enable(1);
	if(iic_ret == -1)
		g_FM50AF_addr_switch = 1;  
	else
		g_FM50AF_addr_switch = 0;
}
