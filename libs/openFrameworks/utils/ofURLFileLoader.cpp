#include "ofURLFileLoader.h"
#include "ofBaseTypes.h"
#include "ofAppRunner.h"
#include "ofUtils.h"

#include "ofConstants.h"

#if !defined(TARGET_IMPLEMENTS_URL_LOADER) && OF_USE_POCO
	#include "Poco/Net/HTTPSession.h"
	#include "Poco/Net/HTTPClientSession.h"
	#include "Poco/Net/HTTPSClientSession.h"
	#include "Poco/Net/HTTPRequest.h"
	#include "Poco/Net/HTTPResponse.h"
	#include "Poco/StreamCopier.h"
	#include "Poco/Path.h"
	#include "Poco/URI.h"
	#include "Poco/Exception.h"
	#include "Poco/URIStreamOpener.h"
	#include "Poco/Net/HTTPStreamFactory.h"
	#include "Poco/Net/HTTPSStreamFactory.h"
	#include "Poco/Net/SSLManager.h"
	#include "Poco/Net/KeyConsoleHandler.h"
	#include "Poco/Net/ConsoleCertificateHandler.h"

	#include "ofThreadChannel.h"
	#include "ofThread.h"

	using namespace Poco::Net;
	using namespace Poco;

	static bool factoryLoaded = false;
#elif OF_USE_CURL
	#include <curl/curl.h>
	#include "ofThreadChannel.h"
	#include "ofThread.h"
	static bool curlInited = false;
#endif

int	ofHttpRequest::nextID = 0;

ofEvent<ofHttpResponse> & ofURLResponseEvent(){
	static ofEvent<ofHttpResponse> * event = new ofEvent<ofHttpResponse>;
	return *event;
}

#if !defined(TARGET_IMPLEMENTS_URL_LOADER) && OF_USE_POCO
class ofURLFileLoaderImpl: public ofThread, public ofBaseURLFileLoader{
public:
	ofURLFileLoaderImpl();
	~ofURLFileLoaderImpl();
    ofHttpResponse get(const string& url);
    int getAsync(const string& url, const string& name=""); // returns id
    ofHttpResponse saveTo(const string& url, const std::filesystem::path& path);
    int saveAsync(const string& url, const std::filesystem::path& path);
	void remove(int id);
	void clear();
    void stop();
    ofHttpResponse handleRequest(const ofHttpRequest & request);
    int handleRequestAsync(const ofHttpRequest& request); // returns id

protected:
	// threading -----------------------------------------------
	void threadedFunction();
    void start();
    void update(ofEventArgs & args);  // notify in update so the notification is thread safe

private:
	// perform the requests on the thread

	ofThreadChannel<ofHttpRequest> requests;
	ofThreadChannel<ofHttpResponse> responses;
	ofThreadChannel<int> cancelRequestQueue;
	set<int> cancelledRequests;
};

ofURLFileLoaderImpl::ofURLFileLoaderImpl() {
	if(!factoryLoaded){
		try {
			HTTPStreamFactory::registerFactory();
			HTTPSStreamFactory::registerFactory();
			SharedPtr<PrivateKeyPassphraseHandler> pConsoleHandler = new KeyConsoleHandler(false);
			SharedPtr<InvalidCertificateHandler> pInvalidCertHandler = new ConsoleCertificateHandler(true);
			Context::Ptr pContext = new Context(Context::CLIENT_USE, "", Context::VERIFY_NONE);
			SSLManager::instance().initializeClient(pConsoleHandler, pInvalidCertHandler, pContext);
			factoryLoaded = true;
		}
		catch (Poco::SystemException & PS) {
			ofLogError("ofURLFileLoader") << "couldn't create factory: " << PS.displayText();
		}
		catch (Poco::ExistsException & PS) {
			ofLogError("ofURLFileLoader") << "couldn't create factory: " << PS.displayText();
		}
	}
}


ofURLFileLoaderImpl::~ofURLFileLoaderImpl(){
	clear();
	stop();
}

ofHttpResponse ofURLFileLoaderImpl::get(const string& url) {
    ofHttpRequest request(url,url);
    return handleRequest(request);
}


int ofURLFileLoaderImpl::getAsync(const string& url, const string& name){
    ofHttpRequest request(url, name.empty() ? url : name);
    requests.send(request);
    start();
    return request.getId();
}


ofHttpResponse ofURLFileLoaderImpl::saveTo(const string& url, const std::filesystem::path& path){
    ofHttpRequest request(url,path.string(),true);
    return handleRequest(request);
}

int ofURLFileLoaderImpl::saveAsync(const string& url, const std::filesystem::path& path){
    ofHttpRequest request(url,path.string(),true);
	requests.send(request);
	start();
	return request.getId();
}

void ofURLFileLoaderImpl::remove(int id){
	cancelRequestQueue.send(id);
}

void ofURLFileLoaderImpl::clear(){
	ofHttpResponse resp;
	ofHttpRequest req;
	while(requests.tryReceive(req)){}
	while(responses.tryReceive(resp)){}
}

void ofURLFileLoaderImpl::start() {
     if (!isThreadRunning()){
		ofAddListener(ofEvents().update,this,&ofURLFileLoaderImpl::update);
        startThread();
    }
}

void ofURLFileLoaderImpl::stop() {
    stopThread();
    requests.close();
    responses.close();
    waitForThread();
}

void ofURLFileLoaderImpl::threadedFunction() {
	setThreadName("ofURLFileLoader " + ofToString(getThreadId()));
	while( isThreadRunning() ){
		int cancelled;
		while(cancelRequestQueue.tryReceive(cancelled)){
			cancelledRequests.insert(cancelled);
		}
		ofHttpRequest request;
		if(requests.receive(request)){
			if(cancelledRequests.find(request.getId())==cancelledRequests.end()){
				ofHttpResponse response(handleRequest(request));
				int status = response.status;
				if(!responses.send(move(response))){
					break;
				}
				if(status==-1){
					// retry
					requests.send(request);
				}
			}else{
				cancelledRequests.erase(cancelled);
			}
		}else{
			break;
		}
	}
}

ofHttpResponse ofURLFileLoaderImpl::handleRequest(const ofHttpRequest & request) {
	try {
		URI uri(request.url);
		std::string path(uri.getPathAndQuery());
		if (path.empty()) path = "/";
		std::string pocoMethod;
		if(request.method==ofHttpRequest::GET){
			pocoMethod = HTTPRequest::HTTP_GET;
		}else{
			pocoMethod = HTTPRequest::HTTP_POST;
		}
		HTTPRequest req(pocoMethod, path, HTTPMessage::HTTP_1_1);
        for(map<string,string>::const_iterator it = request.headers.cbegin(); it!=request.headers.cend(); it++){
			req.add(it->first,it->second);
		}
		HTTPResponse res;
		std::unique_ptr<HTTPClientSession> session;
		if(uri.getScheme()=="https"){
			 //const Poco::Net::Context::Ptr context( new Poco::Net::Context( Poco::Net::Context::CLIENT_USE, "", "", "rootcert.pem" ) );
			session.reset(new HTTPSClientSession(uri.getHost(), uri.getPort()));//,context);
		}else{
			session.reset(new HTTPClientSession(uri.getHost(), uri.getPort()));
		}
        if(request.timeout > 0){
            session->setTimeout(Poco::Timespan(request.timeout,0));
        }
		if(request.contentType!=""){
			req.setContentType(request.contentType);
		}
		if(request.body!=""){
			req.setContentLength( request.body.length() );
			auto & send = session->sendRequest(req);
			send.write(request.body.c_str(), request.body.size());
			send << std::flush;
		}else{
			session->sendRequest(req);
		}

		auto & rs = session->receiveResponse(res);
		if(!request.saveTo){
			return ofHttpResponse(request,rs,res.getStatus(),res.getReason());
		}else{
			ofFile saveTo(request.name,ofFile::WriteOnly,true);
			char aux_buffer[1024];
			rs.read(aux_buffer, 1024);
			std::streamsize n = rs.gcount();
			while (n > 0){
				// we resize to size+1 initialized to 0 to have a 0 at the end for strings
				saveTo.write(aux_buffer,n);
				if (rs.good()){
					rs.read(aux_buffer, 1024);
					n = rs.gcount();
				}
				else n = 0;
			}
			return ofHttpResponse(request,res.getStatus(),res.getReason());
		}

	} catch (const Exception& exc) {
        ofLogError("ofURLFileLoader") << "handleRequest(): "+ exc.displayText();

        return ofHttpResponse(request,-1,exc.displayText());

    } catch (...) {
    	return ofHttpResponse(request,-1,"ofURLFileLoader: fatal error, couldn't catch Exception");
    }

	return ofHttpResponse(request,-1,"ofURLFileLoader: fatal error, couldn't catch Exception");
	
}


int ofURLFileLoaderImpl::handleRequestAsync(const ofHttpRequest& request){
	requests.send(request);
	start();
	return request.getId();
}

void ofURLFileLoaderImpl::update(ofEventArgs & args){
	ofHttpResponse response;
	while(responses.tryReceive(response)){
		try{
			response.request.done(response);
		}catch(...){

		}

		ofNotifyEvent(ofURLResponseEvent(),response);
	}

}

ofURLFileLoader::ofURLFileLoader()
:impl(new ofURLFileLoaderImpl){}

#elif defined(TARGET_EMSCRIPTEN)
#include "ofxEmscriptenURLFileLoader.h"
ofURLFileLoader::ofURLFileLoader()
:impl(new ofxEmscriptenURLFileLoader){}

#elif OF_USE_CURL
class ofURLFileLoaderImpl: public ofThread, public ofBaseURLFileLoader{
public:
	ofURLFileLoaderImpl();
	~ofURLFileLoaderImpl();
	ofHttpResponse get(const string& url);
	int getAsync(const string& url, const string& name=""); // returns id
	ofHttpResponse saveTo(const string& url, const std::filesystem::path& path);
	int saveAsync(const string& url, const std::filesystem::path& path);
	void remove(int id);
	void clear();
	void stop();
	ofHttpResponse handleRequest(const ofHttpRequest & request);
	int handleRequestAsync(const ofHttpRequest& request); // returns id

protected:
	// threading -----------------------------------------------
	void threadedFunction();
	void start();
	void update(ofEventArgs & args);  // notify in update so the notification is thread safe

private:
	// perform the requests on the thread

	ofThreadChannel<ofHttpRequest> requests;
	ofThreadChannel<ofHttpResponse> responses;
	ofThreadChannel<int> cancelRequestQueue;
	set<int> cancelledRequests;
	std::unique_ptr<CURL, void(*)(CURL*)> curl;
};

ofURLFileLoaderImpl::ofURLFileLoaderImpl()
:curl(nullptr, nullptr){
	if(!curlInited){
		 curl_global_init(CURL_GLOBAL_ALL);
	}
	curl = std::unique_ptr<CURL, void(*)(CURL*)>(curl_easy_init(), curl_easy_cleanup);
}

ofURLFileLoaderImpl::~ofURLFileLoaderImpl(){
	clear();
	stop();
}

ofHttpResponse ofURLFileLoaderImpl::get(const string& url) {
	ofHttpRequest request(url,url);
	return handleRequest(request);
}


int ofURLFileLoaderImpl::getAsync(const string& url, const string& name){
	ofHttpRequest request(url, name.empty() ? url : name);
	requests.send(request);
	start();
	return request.getId();
}


ofHttpResponse ofURLFileLoaderImpl::saveTo(const string& url, const std::filesystem::path& path){
	ofHttpRequest request(url,path.string(),true);
	return handleRequest(request);
}

int ofURLFileLoaderImpl::saveAsync(const string& url, const std::filesystem::path& path){
	ofHttpRequest request(url,path.string(),true);
	requests.send(request);
	start();
	return request.getId();
}

void ofURLFileLoaderImpl::remove(int id){
	cancelRequestQueue.send(id);
}

void ofURLFileLoaderImpl::clear(){
	ofHttpResponse resp;
	ofHttpRequest req;
	while(requests.tryReceive(req)){}
	while(responses.tryReceive(resp)){}
}

void ofURLFileLoaderImpl::start() {
	 if (!isThreadRunning()){
		ofAddListener(ofEvents().update,this,&ofURLFileLoaderImpl::update);
		startThread();
	}
}

void ofURLFileLoaderImpl::stop() {
	stopThread();
	requests.close();
	responses.close();
	waitForThread();
}

void ofURLFileLoaderImpl::threadedFunction() {
	setThreadName("ofURLFileLoader " + ofToString(getThreadId()));
	while( isThreadRunning() ){
		int cancelled;
		while(cancelRequestQueue.tryReceive(cancelled)){
			cancelledRequests.insert(cancelled);
		}
		ofHttpRequest request;
		if(requests.receive(request)){
			if(cancelledRequests.find(request.getId())==cancelledRequests.end()){
				ofHttpResponse response(handleRequest(request));
				int status = response.status;
				if(!responses.send(move(response))){
					break;
				}
				if(status==-1){
					// retry
					requests.send(request);
				}
			}else{
				cancelledRequests.erase(cancelled);
			}
		}else{
			break;
		}
	}
}

namespace{
	size_t saveToFile_cb(void *buffer, size_t size, size_t nmemb, void *userdata){
		auto saveTo = (ofFile*)userdata;
		saveTo->write((const char*)buffer, size * nmemb);
		return size * nmemb;
	}

	size_t saveToMemory_cb(void *buffer, size_t size, size_t nmemb, void *userdata){
		auto response = (ofHttpResponse*)userdata;
		response->data.append((const char*)buffer, size * nmemb);
		return size * nmemb;
	}

    size_t readBody_cb(void *ptr, size_t size, size_t nmemb, void *userdata){
        auto body = (std::string*)userdata;

        if(size*nmemb < 1){
            return 0;
        }

        if(!body->empty()) {
            auto sent = std::min(size * nmemb, body->size());
            memcpy(ptr, body->c_str(), sent);
            *body = body->substr(sent);
            return sent;
        }

        return 0;                          /* no more data left to deliver */
    }
}

ofHttpResponse ofURLFileLoaderImpl::handleRequest(const ofHttpRequest & request) {
	curl_slist *headers = nullptr;
	curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());

	// always follow redirections
	curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);

	// Set content type and any other header
	if(request.contentType!=""){
		headers = curl_slist_append(headers, ("Content-Type: " + request.contentType).c_str());
	}
	for(map<string,string>::const_iterator it = request.headers.cbegin(); it!=request.headers.cend(); it++){
		headers = curl_slist_append(headers, (it->first + ": " +it->second).c_str());
	}

	curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);

    std::string body = request.body;

	// set body if there's any
	if(request.body!=""){
		curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, request.body.size());
        //curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, readBody_cb);
        curl_easy_setopt(curl.get(), CURLOPT_READDATA, &body);
	}else{
		curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 0L);
		curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, 0);
        //curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, nullptr);
        curl_easy_setopt(curl.get(), CURLOPT_READDATA, nullptr);
	}
	if(request.method == ofHttpRequest::GET){
		curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1);
	}else{
		curl_easy_setopt(curl.get(), CURLOPT_POST, 1);
	}

    if(request.timeoutSeconds>0){
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, request.timeoutSeconds);
    }

	// start request and receive response
	ofHttpResponse response(request, 0, "");
	auto err = 0;
	if(request.saveTo){
		ofFile saveTo(request.name, ofFile::WriteOnly, true);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &saveTo);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, saveToFile_cb);
		err = curl_easy_perform(curl.get());
	}else{
		curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, saveToMemory_cb);
		err = curl_easy_perform(curl.get());
	}
	if(err==0){
		long http_code = 0;
		curl_easy_getinfo (curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
		response.status = http_code;
	}else{
		response.status = -1;
	}

	if(headers){
		curl_slist_free_all(headers);
	}

	return response;
}


int ofURLFileLoaderImpl::handleRequestAsync(const ofHttpRequest& request){
	requests.send(request);
	start();
	return request.getId();
}

void ofURLFileLoaderImpl::update(ofEventArgs & args){
	ofHttpResponse response;
	while(responses.tryReceive(response)){
		try{
			response.request.done(response);
		}catch(...){

		}

		ofNotifyEvent(ofURLResponseEvent(),response);
	}

}

ofURLFileLoader::ofURLFileLoader()
:impl(new ofURLFileLoaderImpl){}
#endif

ofHttpResponse ofURLFileLoader::get(const string& url){
	return impl->get(url);
}

int ofURLFileLoader::getAsync(const string& url, const string& name){
	return impl->getAsync(url,name);
}

ofHttpResponse ofURLFileLoader::saveTo(const string& url, const std::filesystem::path & path){
	return impl->saveTo(url,path);
}

int ofURLFileLoader::saveAsync(const string& url, const std::filesystem::path & path){
	return impl->saveAsync(url,path);
}

void ofURLFileLoader::remove(int id){
	impl->remove(id);
}

void ofURLFileLoader::clear(){
	impl->clear();
}

void ofURLFileLoader::stop(){
	impl->stop();
}

ofHttpResponse ofURLFileLoader::handleRequest(const ofHttpRequest & request){
	return impl->handleRequest(request);
}

int ofURLFileLoader::handleRequestAsync(const ofHttpRequest& request){
	return impl->handleRequestAsync(request);
}

static bool initialized = false;
static ofURLFileLoader & getFileLoader(){
	static ofURLFileLoader * fileLoader = new ofURLFileLoader;
	initialized = true;
	return *fileLoader;
}


ofHttpRequest::ofHttpRequest()
:saveTo(false)
,method(GET)
,id(nextID++)
{
}

ofHttpRequest::ofHttpRequest(const string& url, const string& name,bool saveTo)
:url(url)
,name(name)
,saveTo(saveTo)
,method(GET)
,id(nextID++)
{
}

int ofHttpRequest::getId() const {
	return id;
}

int ofHttpRequest::getID(){
	return id;
}


ofHttpResponse::ofHttpResponse()
:status(0)
{
}

ofHttpResponse::ofHttpResponse(const ofHttpRequest& request, const ofBuffer& data, int status, const string& error)
:request(request)
,data(data)
,status(status)
,error(error)
{
}

ofHttpResponse::ofHttpResponse(const ofHttpRequest& request, int status, const string& error)
:request(request)
,status(status)
,error(error)
{
}

ofHttpResponse::operator ofBuffer&(){
	return data;
}



ofHttpResponse ofLoadURL(const string& url){
	return getFileLoader().get(url);
}

int ofLoadURLAsync(const string&  url, const string&  name){
	return getFileLoader().getAsync(url,name);
}

ofHttpResponse ofSaveURLTo(const string& url, const string& path){
	return getFileLoader().saveTo(url,path);
}

int ofSaveURLAsync(const string& url, const string& path){
	return getFileLoader().saveAsync(url,path);
}

void ofRemoveURLRequest(int id){
	getFileLoader().remove(id);
}

void ofRemoveAllURLRequests(){
	getFileLoader().clear();
}

void ofStopURLLoader(){
	getFileLoader().stop();
}

void ofURLFileLoaderShutdown(){
	if(initialized){
		ofRemoveAllURLRequests();
		ofStopURLLoader();
		#if !defined(TARGET_IMPLEMENTS_URL_LOADER) && OF_USE_POCO
			Poco::Net::uninitializeSSL();
		#endif
	}
}
