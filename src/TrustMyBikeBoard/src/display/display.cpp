#include "display/display.h"
void display_init(){
    Heltec.display->clear();        
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
    Heltec.display->display();

}
void display_message(const char* message){
    Heltec.display->clear();
    Heltec.display->setFont(ArialMT_Plain_16);
    Heltec.display->drawString(0, 0, message);
    Heltec.display->display();
}
void display_off(){


}