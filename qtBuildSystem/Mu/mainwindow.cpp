#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDir>
#include <QFileDialog>
#include <QTimer>
#include <QTouchEvent>
#include <QMessageBox>
#include <QSettings>
#include <QFont>
#include <QIcon>
#include <QKeyEvent>
#include <QGraphicsScene>
#include <QPixmap>

#include <chrono>
#include <thread>
#include <atomic>
#include <stdint.h>

#include "debugviewer.h"
#include "fileaccess.h"
#include "src/emulator.h"


uint32_t  screenWidth;
uint32_t  screenHeight;
input_t   frontendInput;
QSettings settings;

static QImage            video;
static QTimer*           refreshDisplay;
static DebugViewer*      emuDebugger;
static std::thread       emuThread;
static std::atomic<bool> emuThreadJoin;
static std::atomic<bool> emuOn;
static std::atomic<bool> emuPaused;
static std::atomic<bool> emuInited;
static std::atomic<bool> emuDebugEvent;
static std::atomic<bool> emuNewFrameReady;
static uint16_t*         emuDoubleBuffer;


#if defined(EMU_DEBUG) && defined(EMU_CUSTOM_DEBUG_LOG_HANDLER)
#include <vector>
#include <string>


std::vector<std::string> debugStrings;
std::vector<uint64_t>    duplicateCallCount;
uint32_t                 frontendDebugStringSize;
char*                    frontendDebugString;


void frontendHandleDebugPrint(){
   std::string newDebugString = frontendDebugString;

   //this debug handler doesnt need the \n
   if(newDebugString.back() == '\n')
      newDebugString.pop_back();

   if(!debugStrings.empty() && newDebugString == debugStrings.back()){
      duplicateCallCount.back()++;
   }
   else{
      debugStrings.push_back(newDebugString);
      duplicateCallCount.push_back(1);
   }
}

void frontendInitDebugPrintHandler(){
   frontendDebugString = new char[200];
   frontendDebugStringSize = 200;
}

void frontendFreeDebugPrintHandler(){
   delete[] frontendDebugString;
   frontendDebugStringSize = 0;
   debugStrings.clear();
   duplicateCallCount.clear();
}
#endif


void emuThreadRun(){
   while(!emuThreadJoin){
      if(emuOn){
         emuPaused = false;
         palmInput = frontendInput;
#if defined(EMU_DEBUG)
         if(emulateUntilDebugEventOrFrameEnd()){
            //debug event occured
            emuOn = false;
            emuPaused = true;
            emuDebugEvent = true;
         }
#else
         emulateFrame();
#endif
         if(!emuNewFrameReady){
            memcpy(emuDoubleBuffer, screenWidth == 320 ? palmExtendedFramebuffer : palmFramebuffer, screenWidth * screenHeight * sizeof(uint16_t));
            emuNewFrameReady = true;
         }
      }
      else{
         emuPaused = true;
      }

      std::this_thread::sleep_for(std::chrono::microseconds(16666));
   }
}

static inline void waitForEmuPaused(){
   while(!emuPaused && emuInited)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
}


MainWindow::MainWindow(QWidget* parent) :
   QMainWindow(parent),
   ui(new Ui::MainWindow){
   ui->setupUi(this);

   emuDebugger = new DebugViewer(this);
   refreshDisplay = new QTimer(this);

   ui->calender->installEventFilter(this);
   ui->addressBook->installEventFilter(this);
   ui->todo->installEventFilter(this);
   ui->notes->installEventFilter(this);

   ui->up->installEventFilter(this);
   ui->down->installEventFilter(this);
   ui->left->installEventFilter(this);
   ui->right->installEventFilter(this);
   ui->center->installEventFilter(this);

   ui->power->installEventFilter(this);

   ui->screenshot->installEventFilter(this);
   ui->ctrlBtn->installEventFilter(this);
   ui->debugger->installEventFilter(this);

   ui->ctrlBtn->setIcon(QIcon(":/buttons/images/play.png"));

#if defined(Q_OS_ANDROID)
   if(settings.value("resourceDirectory", "").toString() == "")
      settings.setValue("resourceDirectory", "/sdcard/Mu");
#elif defined(Q_OS_IOS)
   if(settings.value("resourceDirectory", "").toString() == "")
      settings.setValue("resourceDirectory", "/var/mobile/Media");
#else
   if(settings.value("resourceDirectory", "").toString() == "")
      settings.setValue("resourceDirectory", QDir::homePath() + "/Mu");
#endif

   emuThreadJoin = false;
   emuOn = false;
   emuPaused = false;
   emuInited = false;
   emuDebugEvent = false;
   emuNewFrameReady = false;
   emuDoubleBuffer = NULL;

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
   ui->debugger->hide();
#endif

#if defined(EMU_DEBUG) && defined(EMU_CUSTOM_DEBUG_LOG_HANDLER)
   frontendInitDebugPrintHandler();
#endif

   connect(refreshDisplay, SIGNAL(timeout()), this, SLOT(updateDisplay()));
   refreshDisplay->start(16);//update display every 16.67miliseconds = 60 * second
}

MainWindow::~MainWindow(){
#if defined(EMU_DEBUG) && defined(EMU_CUSTOM_DEBUG_LOG_HANDLER)
   frontendFreeDebugPrintHandler();
#endif
   emuThreadJoin = true;
   emuOn = false;
   if(emuThread.joinable())
      emuThread.join();
   if(emuInited){
      emulatorExit();
      delete[] emuDoubleBuffer;
   }
   delete ui;
}

void MainWindow::popupErrorDialog(QString error){
   QMessageBox::critical(this, "Mu", error, QMessageBox::Ok);
}

void MainWindow::popupInformationDialog(QString info){
   QMessageBox::information(this, "Mu", info, QMessageBox::Ok);
}

bool MainWindow::eventFilter(QObject *object, QEvent *event){
   if(QString(object->metaObject()->className()) == "QPushButton" && event->type() == QEvent::Resize){
      QPushButton* button = (QPushButton*)object;
      button->setIconSize(QSize(button->size().width() / 1.7, button->size().height() / 1.7));
   }

   return QMainWindow::eventFilter(object, event);
}

void MainWindow::selectHomePath(){
   QString dir = QFileDialog::getOpenFileName(this, "New Home Directory \"~/Mu\" is default", QDir::root().path(), 0);
   settings.setValue("resourceDirectory", dir);
}

void MainWindow::on_install_pressed(){
   QString app = QFileDialog::getOpenFileName(this, "Open Prc/Pdb/Pqa", QDir::root().path(), 0);
   uint32_t error;
   buffer_t appData;
   appData.data = getFileBuffer(app, appData.size, error);
   if(appData.data){
      error = emulatorInstallPrcPdb(appData);
      delete[] appData.data;
   }

   if(error != FILE_ERR_NONE)
      popupErrorDialog("Could not install app");
}

//display
void MainWindow::updateDisplay(){
   if(emuOn && emuNewFrameReady){
      video = QImage((uchar*)emuDoubleBuffer, screenWidth, screenHeight, QImage::Format_RGB16);
      ui->display->setPixmap(QPixmap::fromImage(video).scaled(QSize(ui->display->size().width() * 0.95, ui->display->size().height() * 0.95), Qt::KeepAspectRatio, Qt::SmoothTransformation));
      ui->display->update();
      emuNewFrameReady = false;
   }

   if(emuDebugEvent){
      //emuThread cant set GUI parameters on its own because its not part of the class
      ui->ctrlBtn->setIcon(QIcon(":/buttons/images/play.png"));
      emuDebugEvent = false;
   }
}

//palm buttons
void MainWindow::on_power_pressed(){
   frontendInput.buttonPower = true;
}

void MainWindow::on_power_released(){
   frontendInput.buttonPower = false;
}

void MainWindow::on_calender_pressed(){
   frontendInput.buttonCalender = true;
}

void MainWindow::on_calender_released(){
   frontendInput.buttonCalender = false;
}

void MainWindow::on_addressBook_pressed(){
   frontendInput.buttonAddress = true;
}

void MainWindow::on_addressBook_released(){
   frontendInput.buttonAddress = false;
}

void MainWindow::on_todo_pressed(){
   frontendInput.buttonTodo = true;
}

void MainWindow::on_todo_released(){
   frontendInput.buttonTodo = false;
}

void MainWindow::on_notes_pressed(){
   frontendInput.buttonNotes = true;
}

void MainWindow::on_notes_released(){
   frontendInput.buttonNotes = false;
}

//emu control
void MainWindow::on_ctrlBtn_clicked(){
   if(!emuOn && !emuInited){
      //start emu
      uint32_t error;
      buffer_t romBuff;
      buffer_t bootBuff;
      romBuff.data = getFileBuffer(settings.value("resourceDirectory", "").toString() + "/palmos41-en-m515.rom", romBuff.size, error);
      if(error != FILE_ERR_NONE){
         popupErrorDialog("Cant load ROM file, error:" + QString::number(error) + ", cant run!");
         return;
      }
      bootBuff.data = getFileBuffer(settings.value("resourceDirectory", "").toString() + "/bootloader-en-m515.rom", bootBuff.size, error);
      if(error != FILE_ERR_NONE){
         //its ok if the bootloader gives an error, the emu doesnt actually need it
         bootBuff.data = NULL;
         bootBuff.size = 0;
      }

      error = emulatorInit(romBuff, bootBuff, FEATURE_ACCURATE);
      delete[] romBuff.data;
      if(bootBuff.data)
         delete[] bootBuff.data;

      if(error == EMU_ERROR_NONE){
         if(palmExtendedFramebuffer != NULL){
            screenWidth = 320;
            screenHeight = 320 + 120;
         }
         else{
            screenWidth = 160;
            screenHeight = 160 + 60;
         }

         frontendInput = palmInput;

         emuThreadJoin = false;
         emuInited = true;
         emuOn = true;
         emuPaused = false;
         emuNewFrameReady = false;
         emuDoubleBuffer = new uint16_t[screenWidth * screenHeight];
         emuThread = std::thread(emuThreadRun);

         ui->calender->setEnabled(true);
         ui->addressBook->setEnabled(true);
         ui->todo->setEnabled(true);
         ui->notes->setEnabled(true);

         ui->up->setEnabled(true);
         ui->down->setEnabled(true);

         ui->power->setEnabled(true);

         /*
         //if FEATURE_EMU_EXT_KEYS enabled add OS 5 buttons
         ui->left->setEnabled(true);
         ui->right->setEnabled(true);
         ui->center->setEnabled(true);
         */

         ui->ctrlBtn->setIcon(QIcon(":/buttons/images/pause.png"));
      }
      else{
         popupErrorDialog("Emu error:" + QString::number(error) + ", cant run!");
      }
   }
   else if(emuOn){
      emuOn = false;
      ui->ctrlBtn->setIcon(QIcon(":/buttons/images/play.png"));
   }
   else if(!emuOn){
      emuOn = true;
      ui->ctrlBtn->setIcon(QIcon(":/buttons/images/pause.png"));
   }
}

void MainWindow::on_debugger_clicked(){
   if(emuInited){
      if(emuOn){
         emuOn = false;
         ui->ctrlBtn->setIcon(QIcon(":/buttons/images/play.png"));
      }

      waitForEmuPaused();

      emuDebugger->exec();
   }
   else{
      popupInformationDialog("Cant open debugger, emulator not running.");
   }
}

void MainWindow::on_screenshot_clicked(){
   uint64_t screenshotNumber = settings.value("screenshotNum", 0).toLongLong();
   QString path = settings.value("resourceDirectory", "").toString();
   QDir location = path + "/screenshots";

   if(!location.exists())
      location.mkpath(".");

   video.save(path + "/screenshots/" + "screenshot" + QString::number(screenshotNumber, 10) + ".png", NULL, 100);
   screenshotNumber++;
   settings.setValue("screenshotNum", screenshotNumber);
}
