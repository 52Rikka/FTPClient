#include "client.h"
#include <io.h> //C库
using namespace std;

Client::Client() {
    infoThread = new InfoThread;
}

Client::~Client() {
    //当在connect状态直接关闭程序的时候，将会通过析构函数来关闭socket
    if(flag!=0){ //没有主动调用disconnect
        closesocket(dataSocket);
        closesocket(controlSocket);
        WSACleanup();
    }
    delete infoThread;
    delete [] buf;
    delete [] databuf;
}

void Client::login(QString ip_addr, QString username, QString password) {
    this->ip_addr = ip_addr.toStdString();
    this->username = username.toStdString();
    this->password = password.toStdString();
}

int Client::connectServer() {
    WSADATA dat;
    int ret;

    //初始化，很重要
    if (WSAStartup(MAKEWORD(2,2),&dat)!=0)  //Windows Sockets Asynchronous启动
    {
        cout<<"Init Failed: "<<GetLastError()<<endl;
        infoThread->sendInfo("Init Failed!\n");
        return -1;
    }
    flag = -1;
    //创建controlSocket
    controlSocket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(controlSocket==INVALID_SOCKET)
    {
        cout<<"Creating Control Socket Failed: "<<GetLastError()<<endl;
        infoThread->sendInfo("Creating Control Socket Failed.\n");
        return -1;
    }
    //构建服务器访问参数结构体
    serverAddr.sin_family=AF_INET;
    serverAddr.sin_addr.S_un.S_addr=inet_addr(ip_addr.c_str()); //地址
    serverAddr.sin_port=htons(PORT);            //端口
    memset(serverAddr.sin_zero,0,sizeof(serverAddr.sin_zero));

    //连接
    ret=connect(controlSocket,(struct sockaddr*)&serverAddr,sizeof(serverAddr));
    if(ret==SOCKET_ERROR)
    {
        cout<<"Control Socket Connecting Failed: "<<GetLastError()<<endl;
        infoThread->sendInfo("Control Socket Connecting Failed\n");
        return -1;
    }
    cout<<"Control Socket connecting is success."<<endl;
    //接收返回状态信息
    Sleep(300); //?是否多次一举
    recvControl(220);

    //输入用户名
    executeCmd("USER " + username);
    recvControl(331);

    //输入密码
    executeCmd("PASS " + password);
    recvControl(230);

    listPwd();
    return 0;
}

int Client::disconnect() {
    executeCmd("QUIT");
    recvControl(221);
    ip_addr="", username="", password="", INFO= "";
    filelist.clear(); //filelist是个什么东西
    memset(buf, 0, BUFLEN);
    memset(databuf, 0, DATABUFLEN);
    closesocket(dataSocket);
    closesocket(controlSocket);
    flag = WSACleanup();
    return 0;
    //if(flag==0) cout << "close socket successfully\n";
}

int Client::changeDir(string tardir) {
    memset(buf, 0, BUFLEN);
    executeCmd("CWD "+tardir);
    recvControl(250);
    listPwd();
    return 0;
}
void Client::getDownloadedLength(string fileName){
    downloadedFileLength = 0;
    ifstream ifile;
    ifile.open(fileName,ios::binary);
    if(ifile){
        ifile.seekg(0,ifile.end);
        int length = ifile.tellg();
        downloadedFileLength = length>0?length:0;
        ifile.close();
    }

}

int Client::downFile(string remoteName, string localDir){
    string localFile = localDir + "/" + remoteName;
    ofstream ofile;
    long long int remoteFileSize = getFileSize(remoteName);
    getDownloadedLength(localFile);

    if(downloadedFileLength == remoteFileSize){
        infoThread->sendInfo("file had been downloaded!\n");
        cout << "file had been downloaded!\n";
        return 0;
    }else if(downloadedFileLength < remoteFileSize && downloadedFileLength >=0 ){
        //打开断点续传
        ofile.open(localFile, ios::binary | ios::app);
        intoPasv(); //每次下载文件都要进入被动模式(重新获取一个数据端口） 完成下载后关闭数据连接
        executeCmd("TYPE I");
        recvControl(200);
        executeCmd("REST "+to_string(downloadedFileLength));
        recvControl(350);
        infoThread->updateDownloadProcess(downloadedFileLength * 100/remoteFileSize);
        infoThread->showProcessBar(); //显示下载进度
        executeCmd("RETR "+remoteName);
        recvControl(150);
        memset(databuf, 0, DATABUFLEN);
        int ret = recv(dataSocket, databuf, DATABUFLEN, 0); int count=0;
        while(ret>0) //每次从dataSocket中取出来ret个字节放到DATA缓冲里面
        {
            if(!isRuningTask()) {
                //如果任务被中止
                closesocket(dataSocket); //主动关闭数据socket
                recvControl(426);
                ofile.close();
                infoThread->hideProcessBar();
                disconnect();
                return -1;
            }
            ofile.write(databuf, ret);
            ret = recv(dataSocket, databuf, DATABUFLEN, 0);
            downloadedFileLength += ret;
            count ++;
            if(count == 50){
                infoThread->updateDownloadProcess(downloadedFileLength * 100/remoteFileSize);
                count = 0;
            }
        }
        ofile.close();
        infoThread->sendInfo(remoteName+" has been downloaded.\n");
        cout << remoteName+" has been downloaded.\n";
        closesocket(dataSocket);
        recvControl(226);
        infoThread->hideProcessBar();//下载完成后关闭下载进度条s
        return 0;
    }
}

int Client::getRemoteFileSize(std::string fname){
    executeCmd("SIZE "+ fname);
    int infolen = recv(controlSocket, buf, BUFLEN, 0); //获取命令返回结果，并存储到buf中
    buf[infolen] = '\0';
    if(getStateCode() != 213) {
        //文件不存在
        return 0;
    }else {
        return getFileSize(fname);
    }
}
int Client::upFile(string localName) { //暂时无法断点续传 无法上传中文路径的文件
    ifstream ifile;
    string remoteName = localName.substr(localName.find_last_of("/")+1);
    getDownloadedLength(localName); //借助下载文件的思路
    long long int targetLength = downloadedFileLength; //将要上传这么多bytes
    long long int remoteFileLength = getRemoteFileSize(remoteName);
    if(remoteFileLength == targetLength){
        infoThread->sendInfo(remoteName + " has been upped ");
        return 0;
    }else if(remoteFileLength < targetLength && remoteFileLength >=0){
        //启动断点续传
        intoPasv();
        ifile.open(localName,ios::binary); //二进制方式打开文件
        if(ifile) { cout << "open file successfully\n" ; } else {cout << "open file fail\n";}
        executeCmd("TYPE I");
        recvControl(200);
        executeCmd("APPE "+remoteName);
        recvControl(150);
        ifile.seekg(remoteFileLength); //本地文件移动指针
        int count=0;
        infoThread->updateDownloadProcess(remoteFileLength * 100 / targetLength);
        infoThread->showProcessBar(); //显示精度条
        while(!ifile.eof())
        {
            if(!isRuningTask()) {
                //如果任务被中止
                closesocket(dataSocket); //主动关闭数据socket
                recvControl(426);
                ifile.close();
                infoThread->hideProcessBar();
                disconnect();
                return -1;
            }
            ifile.read(databuf,DATABUFLEN);
            int readLength = ifile.gcount(); //成功读出的数据
            send(dataSocket, databuf, readLength, 0);
            remoteFileLength += readLength; //更新上传进度条
            if(count == 50){
                infoThread->updateDownloadProcess(remoteFileLength * 100 /targetLength );
                count = 0;
            }
            count ++;

        }

        ifile.close();
        infoThread->hideProcessBar(); //隐藏进度条
        closesocket(dataSocket);
        recvControl(226);
        listPwd();
        return 0;
    }
}
//private function---------------------------------------------------------
int Client::executeCmd(string cmd) {
    cmd += "\r\n";
    int cmdlen = cmd.size();
    infoThread->sendInfo(cmd);
    cout << cmd;
    send(controlSocket, cmd.c_str(), cmdlen, 0);
    return 0;
}

// TODO:This function needs to be modified so that
// long information could be received.
int Client::recvControl(int stateCode, string errorInfo) {
    if(errorInfo.size()==1)
        errorInfo = "state code error!";
    if(nextInfo.size()==0) {
        int t;
        Sleep(50);
        memset(buf, 0, BUFLEN);
        recvInfo.clear();
        int infolen = recv(controlSocket, buf, BUFLEN, 0);
        if(infolen==BUFLEN) {
            cout << "ERROR! Too long information too receive!" << endl;
            infoThread->sendInfo("ERROR! Too long information too receive!\n");
            return -1;
        }
        buf[infolen] = '\0';
        t = getStateCode();
        recvInfo = buf;
        cout << recvInfo << endl;

        // JUNK
        int f1 = recvInfo.find("\r\n"); //list -al 命令的相应可能有两段， 150 和 226 第一段一定是150
        int f2 = recvInfo.rfind("\r\n"); //是否有第二个回应结果 226
        if(f1!=f2) {
            nextInfo = recvInfo.substr(f1+2);
            cout << "find a junk!" ; //其实这个junk表示，list -al 的回应代码一下发送了两个回应结果
        }

        infoThread->sendInfo(recvInfo); //直接把recvInfo不加处理地发送了
        if(t == stateCode)
            return 0;
        else { //这次没办法了，是真的出错了
            cout << errorInfo << endl;
            infoThread->sendInfo(errorInfo);
            return -1;
        }

    }
    else {
        recvInfo = nextInfo;
        nextInfo.clear();
        return 0;
    }


}

//从返回信息中获取状态码
int Client::getStateCode()
{
    int num=0;
    char* p = buf;
//    cout << "notice here! "<<buf << endl;
    while(p != nullptr)
    {
        num=10*num+(*p)-'0';
        p++;
        if(*p==' ' || *p=='-')
        {
            break;
        }
    }
    return num;
}

//从返回信息“227 Entering Passive Mode (182,18,8,37,10,25).”中
//获取数据端口
int Client::getPortNum()
{
    int num1=0,num2=0;

    char* p=buf;
    int cnt=0;
    while( 1 )
    {
        if(cnt == 4 && (*p) != ',')
        {
            if(*p<='9' && *p>='0')
                num1 = 10*num1+(*p)-'0';
        }
        if(cnt == 5)
        {
            if(*p<='9' && *p>='0')
                num2 = 10*num2+(*p)-'0';
        }
        if((*p) == ',')
        {
            cnt++;
        }
        p++;
        if((*p) == ')')
        {
            break;
        }
    }
    std::cout<<"The data port number is "<<num1*256+num2<<std::endl;
    return num1*256+num2;
}
void Client::updateRemotePath(){
    executeCmd("PWD");
    recvControl(257);
//    cout << "buf now is :" << buf;
    //解析buf中的数据 将path部分展示到界面上
    string result(buf);
    int from = result.find_first_of("\"");
    int to = result.find_last_of("\"");
    string remotePath = result.substr(from+1,to-from-1);
    //cout << "remotePath is :"<< remotePath << endl;
    infoThread->updateRemotePath(remotePath);
}
int Client::listPwd() {
    updateRemotePath();
    intoPasv();
    executeCmd("LIST -al");
    recvControl(150);
    //list -al 的结果将通过数据端口发送
    memset(databuf, 0, DATABUFLEN);
    string fulllist;
    int ret = recv(dataSocket, databuf, DATABUFLEN-1, 0);
    while(ret>0) {
        databuf[ret] = '\0';
        fulllist += databuf;
        ret = recv(dataSocket, databuf, DATABUFLEN-1, 0);
    }
//    cout <<"look here!=>"<< fulllist << endl;
    removeSpace(fulllist);

    int lastp, lastq, p, q;
    vector<string> eachrow;
    string rawrow;
    string item;
    filelist.clear();
    p = fulllist.find("\r\n");
    lastp = 0;
    while(p>=0) {
        eachrow.clear();
        rawrow = fulllist.substr(lastp, p-lastp);

        q = rawrow.find(' ');
        lastq = 0;
        for(int i=0; i<8; i++) {
            item = rawrow.substr(lastq, q-lastq);
            eachrow.push_back(item);
            lastq = q + 1;
            q = rawrow.find(' ', lastq);
        }
        item = rawrow.substr(lastq);
        eachrow.push_back(item);
        filelist.push_back(eachrow);

        lastp = p + 2;
        p = fulllist.find("\r\n", lastp);
    }
    closesocket(dataSocket);
    recvControl(226);
    return 0;
}

int Client::intoPasv() {
    int dataPort, ret;
    //切换到被动模式
    executeCmd("PASV");
    recvControl(227);
    //executeFTPCmd(227, "PASV");                //227

    //返回的信息格式为---h1,h2,h3,h4,p1,p2
    //其中h1,h2,h3,h4为服务器的地址，p1*256+p2为数据端口
    dataPort=getPortNum();
    //客户端数据传输socket
    dataSocket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    serverAddr.sin_port=htons(dataPort);    //更改连接参数中的port值
    ret=connect(dataSocket,(struct sockaddr*)&serverAddr,sizeof(serverAddr));
    if(ret==SOCKET_ERROR)
    {
        cout<<"Data Socket connecting Failed: "<<GetLastError()<<endl;
        return -1;
    }
    cout<<"Data Socket connecting is success."<<endl;
    return 0;
}

int Client::getFileSize(string fname) {
    executeCmd("SIZE " + fname);
    recvControl(213);
    char* p = buf;
    while(p != nullptr && *p != ' ') {
        p++;
    }
    p++;
    int num = 0;
    while(p != nullptr && *p != '\r') {
        num *= 10;
        num += (*p - '0');
        p++;
    }
    memset(buf, 0, BUFLEN);
    //cout << num << " is here";
    return num;

}


void Client::removeSpace(string & src) {
    //空白符只保留一个
    int p, q;
    p = src.find(' ');
    while(p>=0) {
        for(q=p+1; src[q]==' '; q++);
        src.erase(p+1, q-p-1);
        p = src.find(' ', p+1);
    }
}

int Client::deleteFile(string fname) {
    executeCmd("DELE "+fname);
    recvControl(250);
    listPwd();
    return 0;
}

int Client::deleteDir(string dname) {
    executeCmd("RMD "+dname);
    recvControl(250);
    listPwd();
    return 0;
}

int Client::rename(string src, string dst) {
    executeCmd("RNFR "+src);
    recvControl(350);
    executeCmd("RNTO "+dst);
    recvControl(250);
    listPwd();
    return 0;
}

int Client::mkDir(string name) {
    executeCmd("MKD "+name);
    recvControl(250);
    listPwd();
    return 0;
}
