package android.net;

import android.os.Parcel;

import java.util.Collections;
import java.util.List;

/**
 * 纯 JVM 单测专用 Uri(第 9 轮审查补测配套)。
 *
 * android.net.Uri 的构造器是包私有的,测试包里既不能 `object : Uri()` 子类化,
 * mockable android.jar 又不含 StringUri 等私有内部类(反射 Class.forName 直接
 * ClassNotFoundException)。本类放在 android.net 同包下,利用包私有访问权
 * 子类化 Uri,把全部抽象方法实现成「无副作用返回值」—— 测试只把它存进
 * JobSpec/JobRecord 字段,从不真正解封装。
 */
public final class TestUris {
    private TestUris() {}

    public static Uri fake(final String value) {
        return new Uri() {
            @Override public boolean isHierarchical() { return true; }
            @Override public boolean isRelative() { return false; }
            @Override public String getScheme() { return "file"; }
            @Override public String getSchemeSpecificPart() { return value; }
            @Override public String getEncodedSchemeSpecificPart() { return value; }
            @Override public String getAuthority() { return null; }
            @Override public String getEncodedAuthority() { return null; }
            @Override public String getUserInfo() { return null; }
            @Override public String getEncodedUserInfo() { return null; }
            @Override public String getHost() { return null; }
            @Override public int getPort() { return -1; }
            @Override public String getPath() { return value; }
            @Override public String getEncodedPath() { return value; }
            @Override public String getQuery() { return null; }
            @Override public String getEncodedQuery() { return null; }
            @Override public String getFragment() { return null; }
            @Override public String getEncodedFragment() { return null; }
            @Override public List<String> getPathSegments() { return Collections.emptyList(); }
            @Override public String getLastPathSegment() { return value; }
            @Override public String toString() { return value; }
            @Override public Uri.Builder buildUpon() { throw new UnsupportedOperationException(); }
            @Override public int describeContents() { return 0; }
            @Override public void writeToParcel(Parcel out, int flags) {
                throw new UnsupportedOperationException();
            }
            @Override public boolean equals(Object o) { return o == this; }
            @Override public int hashCode() { return value.hashCode(); }
        };
    }

    public static Uri fake() {
        return fake("file:///prf-test");
    }
}
