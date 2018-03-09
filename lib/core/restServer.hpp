#ifndef REST_SERVER_HPP
#define REST_SERVER_HPP

#include "Config.hpp"
#include "mongoose.h"
#include "json.hpp"
#ifdef MAC_OSX
	#include "osxBindCPU.hpp"
#endif
#include <thread>
#include <functional>
#include <map>

using json = nlohmann::json;
using std::map;

#define STATUS_OK 200
#define STATUS_BAD_REQUEST 400
#define STATUS_REQUEST_FAILED 402
#define STATUS_NOT_FOUND 404
#define STATUS_INTERNAL_ERROR 500

class connectionInstance {
public:
    connectionInstance(mg_connection *nc, int ev, void * ev_data);
    ~connectionInstance();

    void send_error(const string &message, int status_code);
    void send_json_reply(json &json_reply);
    void send_binary_reply(uint8_t * data, int len);
    void send_empty_reply(int status_code);

    /**
     * Sends an HTTP response with "content-type" header set to "text/plain"
     *
     * @param[in] reply The body of the reply
     * @param[in] status_code HTTP response status code (default = HTTP_OK = 200)
     */
    void send_text_reply(const string &reply, int status_code=STATUS_OK);

    // TODO use move constructors with this.
    string get_body();
    string get_full_message();
private:
    mg_connection *nc;
    int ev;
    void * ev_data;
};

class restServer {

public:
    restServer();
    virtual ~restServer();

    void start();

    void mongoose_thread();

    /**
     * Registers a callback for a specified HTTP endpoint.
     *
     * @param[in] endpoint Path section of the URL that is handled by the callback
     * @param[in] callback Callback function invoked to handle the request on the endpoint
     *
     * @note Re-registering on an endpoint will override the previous
     * callback value.
     */
    void register_get_callback(string endpoint,
                               std::function<void(connectionInstance &)> callback);
    void register_json_callback(string endpoint,
                        std::function<void(connectionInstance &, json &)> callback);
    std::map<string, std::function<void(connectionInstance &, json &)>> json_callbacks;

    static void handle_request(struct mg_connection *nc, int ev, void *ev_data);

private:
    std::map<string, std::function<void(connectionInstance &)>> get_callbacks;

    // Returns the json parsed object from med.body, in json_parse
    // Returns 0 if the body is valid json.
    // Else returns -1 if the body is invalid json, and sends error message to client in header.
    // Do not call any ms_send calls if this function fails.
    int handle_json(struct mg_connection *nc, int ev, void *ev_data, json &json_parse);

    struct mg_mgr mgr;
    struct mg_connection *nc;
    const char *port = "12048";

    std::thread main_thread;
};

restServer * get_rest_server();

#endif /* REST_SERVER_HPP */