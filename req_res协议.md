<img width="752" alt="07227419a14d2d4ed88aeb5322b851a8" src="https://github.com/20083017/20083017.github.io/assets/8308226/7d1acf52-e272-453c-8dac-3501c5eb073e">


# rest_rpc

## req_res协议

```req_header
序号  | 类型     | name         | 字节数
1    | uint8_t  | MAGIC_NUM    | 0
2    | uint8_t  | req_type     | 1
3    | uint32_t | buffer_size  | 2-5
4    | uint64_t | req_id       | 6-13
5    | uint32_t | func_id      | 14-17
```
注释: 
* MAGIC_NUM 魔数，固定值 39
* req_type ENUM {req_res,sub_pub}
* buffer_size  body的大小
* req_id client发出的第i个请求，i从0开始
* func_id rpc_name的md5值，md5为一种哈希函数，此处采用hash32;rpc_name为register_handler的第一个参数，全局唯一

```req_body
序号  | 类型     | name      | 字节数
1    | uint64_t | req_id    | 0
2    | uint8_t  | req_type  | 1
3    | string   | content   | 
5    | uint32_t | func_id   | 
```
注释: 
* req_id client发出的第i个请求，i从0开始
* req_type ENUM {req_res,sub_pub}
* content  client.call 函数除第一个参数以外，其他参数的msgpack序列化的结果
* func_id rpc_name的md5值，md5为一种哈希函数，此处采用hash32


```res_header
序号  | 类型     | name         | 字节数
1    | uint8_t  | MAGIC_NUM    | 0
2    | uint8_t  | req_type     | 1
3    | uint32_t | buffer_size  | 2-5
4    | uint64_t | req_id       | 6-13
```
注释: 
* MAGIC_NUM 魔数，固定值 39
* req_type ENUM {req_res,sub_pub}
* buffer_size  body的大小
* req_id req_header中的req_id

```req_body
序号  | 类型     | name      | 字节数
1    | uint64_t | req_id    | 0
2    | uint8_t  | req_type  | 1
3    | string   | content   | 
```
注释: 
* req_id req_header中的req_id
* req_type ENUM {req_res,sub_pub}
* content  失败: result_code::FAIL, "unknown function: " + get_name_by_key(key)   
           成功: result_code::OK, result   

## server.register_handler函数

void register_handler(std::string const &name, const Function &f);

register_handler 中第一个参数，即为函数注册名，rpc_name,

## client.call 函数
auto result = client.call<int>("add", 1, 2);

